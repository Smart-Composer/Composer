#include "DerivedPlayback.h"

#include <composer/engine/EngineSetup.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
using composer::app::derivePlayback;
using composer::app::PlaybackConversion;
using composer::project::MidiEvent;
using composer::project::ProjectDocument;

struct ScratchDirectory
{
    const juce::File parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const juce::File directory = parent.getChildFile("composer-playback-" + juce::Uuid().toString());
    ScratchDirectory()
    {
        REQUIRE(directory.isAChildOf(parent));
        REQUIRE_FALSE(directory.exists());
        REQUIRE(directory.createDirectory().wasOk());
    }
    ~ScratchDirectory()
    {
        if (directory.isAChildOf(parent) && directory.getFileName().startsWith("composer-playback"))
            directory.deleteRecursively();
    }
};

struct Fixture
{
    ScratchDirectory scratch;
    std::unique_ptr<tracktion::Engine> engine =
        composer::engine::createHeadlessEngine("ComposerPlaybackTests", scratch.directory);
    std::unique_ptr<tracktion::Edit> edit;

    explicit Fixture(double rate = 48000.0, bool hosted = false)
    {
        if (hosted)
        {
            auto& device = engine->getDeviceManager().getHostedAudioDeviceInterface();
            tracktion::HostedAudioDeviceInterface::Parameters parameters;
            parameters.sampleRate = rate;
            parameters.blockSize = 128;
            parameters.useMidiDevices = false;
            parameters.inputChannels = 0;
            parameters.outputChannels = 2;
            device.initialise(parameters);
            device.prepareToPlay(rate, parameters.blockSize);
            engine->getDeviceManager().dispatchPendingUpdates();
        }
        edit = tracktion::Edit::createSingleTrackEdit(*engine);
        edit->tempoSequence.getTempo(0)->setBpm(composer::project::projectTempoBpm);
        edit->clickTrackEnabled = false;
    }

    tracktion::AudioTrack& track() { return *tracktion::getAudioTracks(*edit)[0]; }
};

std::vector<MidiEvent> exportEvents(const PlaybackConversion& converted)
{
    std::vector<MidiEvent> events;
    for (auto* clip : converted.clips)
        for (const auto* event : clip->getSequence().exportToPlaybackMidiSequence(
                 *clip, tracktion::MidiList::TimeBase::beatsRaw, false))
            events.push_back({ clip->edit.tempoSequence.toTime(
                                  tracktion::BeatPosition::fromBeats(event->message.getTimeStamp())).inSeconds(),
                               { event->message.getRawData(),
                                 event->message.getRawData() + event->message.getRawDataSize() } });
    return events;
}

std::vector<MidiEvent> canonicalValues(std::vector<MidiEvent> events)
{
    std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
        return left.timeSeconds < right.timeSeconds
            || (left.timeSeconds == right.timeSeconds && left.bytes < right.bytes);
    });
    return events;
}

bool hasEvent(const std::vector<MidiEvent>& events, double time, std::initializer_list<std::uint8_t> bytes)
{
    const std::vector<std::uint8_t> wanted(bytes);
    return std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.bytes == wanted && event.timeSeconds == Catch::Approx(time).margin(1.0e-12);
    });
}

class MidiObserver final : public tracktion::Plugin
{
public:
    static constexpr const char* xmlTypeName = "composer_playback_test_observer";
    static const char* getPluginName() { return "Playback MIDI Observer"; }
    explicit MidiObserver(tracktion::PluginCreationInfo info) : Plugin(info) {}
    ~MidiObserver() override { notifyListenersOfDeletion(); }
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override { return true; }
    bool producesAudioWhenNoAudioInput() override { return true; }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    void initialise(const tracktion::PluginInitialisationInfo& info) override { rate = info.sampleRate; }
    void deinitialise() override {}

    struct Event { std::int64_t sample; std::array<std::uint8_t, 3> bytes; int size; };
    std::array<Event, 128> events {};
    std::size_t count = 0;
    std::int64_t renderedEnd = 0;
    bool overflow = false;

    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        if (context.bufferForMidiMessages != nullptr)
            for (const auto& message : *context.bufferForMidiMessages)
            {
                if (message.getRawDataSize() < 2 || message.getRawDataSize() > 3)
                    continue;
                if (count == events.size())
                {
                    overflow = true;
                    continue;
                }
                auto& event = events[count++];
                event.sample = std::llround((context.editTime.getStart().inSeconds()
                    + message.getTimeStamp() + context.midiBufferOffset) * rate);
                event.size = message.getRawDataSize();
                std::copy_n(message.getRawData(), event.size, event.bytes.begin());
            }
        renderedEnd = std::llround(context.editTime.getEnd().inSeconds() * rate);
        if (context.destBuffer != nullptr)
            context.destBuffer->clear(context.bufferStartSample, context.bufferNumSamples);
    }

private:
    double rate = 0.0;
};

std::vector<MidiEvent> playEvents(const ProjectDocument& document, double rate)
{
    Fixture fixture(rate, true);
    const auto original = document;
    const auto converted = derivePlayback(document, fixture.track(), rate);
    REQUIRE(converted.clips.size() <= 16);
    fixture.engine->getPluginManager().createBuiltInType<MidiObserver>();
    auto plugin = fixture.edit->getPluginCache().createNewPlugin(MidiObserver::xmlTypeName, {});
    auto* observer = dynamic_cast<MidiObserver*>(plugin.get());
    REQUIRE(observer != nullptr);
    fixture.track().pluginList.insertPlugin(plugin, 0, nullptr);
    auto& devices = fixture.engine->getDeviceManager();
    auto& transport = fixture.edit->getTransport();
    transport.ensureContextAllocated(true);
    devices.dispatchPendingUpdates();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    transport.setPosition(tracktion::TimePosition());
    transport.play(false);
    REQUIRE(transport.isPlaying());
    juce::AudioBuffer<float> audio(2, 128);
    juce::MidiBuffer midi;
    const auto targetEnd = std::llround((document.durationSeconds + 0.02) * rate);
    for (int callback = 0; callback < 30000 && observer->renderedEnd < targetEnd; ++callback)
    {
        audio.clear();
        midi.clear();
        devices.getHostedAudioDeviceInterface().processBlock(audio, midi);
        if (callback % 256 == 0)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    transport.stop(false, false);
    transport.freePlaybackContext();
    REQUIRE(observer->renderedEnd >= targetEnd);
    REQUIRE_FALSE(observer->overflow);
    CHECK(document == original);
    std::vector<MidiEvent> result;
    for (std::size_t index = 0; index < observer->count; ++index)
    {
        const auto& event = observer->events[index];
        result.push_back({ static_cast<double>(event.sample) / rate,
                           { event.bytes.begin(), event.bytes.begin() + event.size } });
    }
    return result;
}
}

TEST_CASE("Derived playback preserves raw values while separating MIDI channels")
{
    Fixture fixture;
    ProjectDocument document;
    document.patch.gainDb = -18.25;
    document.durationSeconds = 1.0;
    document.events = {
        { 0.125, { 0x92, 69, 100 } }, { 0.125, { 0xb2, 1, 40 } },
        { 0.125, { 0xe2, 23, 67 } }, { 0.125, { 0xd2, 51 } },
        { 0.125, { 0xa2, 69, 52 } }, { 0.125, { 0xc2, 17 } },
        { 0.25, { 0x90, 60, 91 } }, { 0.5, { 0x82, 69, 73 } },
        { 0.75, { 0x80, 60, 45 } }, { 1.0, { 0xcf, 22 } }
    };
    const auto original = document;
    const auto converted = derivePlayback(document, fixture.track(), 48000.0);
    REQUIRE(converted.clips.size() == 3);
    CHECK(converted.retriggerClosures == 0);
    CHECK(converted.terminalClosures == 0);
    CHECK(converted.orphanNoteOffs == 0);
    CHECK(converted.velocityZeroNoteOffs == 0);
    CHECK(converted.suppressedShortNotes == 0);
    const auto exported = exportEvents(converted);
    CHECK(canonicalValues(exported) == canonicalValues(document.events));
    CHECK(document == original);
    for (const auto* clip : converted.clips)
    {
        CHECK_FALSE(clip->getMPEMode());
        CHECK_FALSE(clip->isSendingBankChanges());
        CHECK(clip->getVolumeDb() == 0.0f);
        CHECK(clip->getPosition().getEnd().inSeconds() == document.durationSeconds + 1.0 / 48000.0);
    }
    // The source note precedes equal-time controllers. Tracktion's export puts
    // controllers first; compare values independently rather than claim raw order.
    const auto on = std::find_if(exported.begin(), exported.end(), [](const auto& event) {
        return event.bytes == std::vector<std::uint8_t> { 0x92, 69, 100 };
    });
    const auto controller = std::find_if(exported.begin(), exported.end(), [](const auto& event) {
        return event.bytes == std::vector<std::uint8_t> { 0xb2, 1, 40 };
    });
    REQUIRE(on != exported.end());
    REQUIRE(controller != exported.end());
    CHECK(controller < on);
    CHECK(canonicalValues(playEvents(document, 48000.0)) == canonicalValues(document.events));
}

TEST_CASE("Derived playback reports note pairing changes without rewriting originals")
{
    Fixture fixture;
    ProjectDocument document;
    document.durationSeconds = 2.0;
    document.events = {
        { 0.1, { 0x80, 61, 45 } }, { 0.2, { 0x90, 60, 90 } },
        { 0.3, { 0x90, 60, 91 } }, { 0.4, { 0x80, 60, 46 } },
        { 0.5, { 0x80, 60, 47 } }, { 0.6, { 0x90, 62, 92 } },
        { 0.7, { 0x90, 62, 0 } }, { 0.8, { 0x90, 63, 93 } },
        { 1.0, { 0x90, 64, 94 } }, { 1.0, { 0x80, 64, 48 } }
    };
    const auto original = document;
    const auto converted = derivePlayback(document, fixture.track(), 48000.0);
    CHECK(converted.retriggerClosures == 1);
    CHECK(converted.terminalClosures == 1);
    CHECK(converted.orphanNoteOffs == 2);
    CHECK(converted.velocityZeroNoteOffs == 1);
    CHECK(converted.suppressedShortNotes == 1);
    const auto exported = exportEvents(converted);
    REQUIRE(exported.size() == 8);
    CHECK(hasEvent(exported, 0.3, { 0x80, 60, 0 }));
    CHECK(hasEvent(exported, 0.4, { 0x80, 60, 46 }));
    CHECK(hasEvent(exported, 0.7, { 0x80, 62, 0 }));
    CHECK(hasEvent(exported, 2.0, { 0x80, 63, 0 }));
    CHECK(document == original);
}

TEST_CASE("Derived playback rejects invalid setup before adding clips")
{
    Fixture fixture;
    ProjectDocument document;
    document.durationSeconds = 1.0;
    document.events = { { 0.0, { 0xb0, 7, 99 } } };
    const auto original = document;
    for (const double rate : { 0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::denorm_min(),
                               std::numeric_limits<double>::max() })
    {
        CHECK_THROWS_AS(derivePlayback(document, fixture.track(), rate), std::invalid_argument);
        CHECK(fixture.track().getClips().isEmpty());
    }
    auto invalid = document;
    invalid.events[0].bytes = { 0x90, 0xff, 100 };
    CHECK_THROWS_AS(derivePlayback(invalid, fixture.track(), 48000.0), std::invalid_argument);
    CHECK(fixture.track().getClips().isEmpty());
    fixture.edit->tempoSequence.getTempo(0)->setBpm(90.0);
    CHECK_THROWS_AS(derivePlayback(document, fixture.track(), 48000.0), std::invalid_argument);
    CHECK(fixture.track().getClips().isEmpty());
    fixture.edit->tempoSequence.getTempo(0)->setBpm(120.0);
    const auto converted = derivePlayback(document, fixture.track(), 48000.0);
    REQUIRE(converted.clips.size() == 1);
    CHECK_THROWS_AS(derivePlayback(document, fixture.track(), 48000.0), std::invalid_argument);
    CHECK(fixture.track().getClips().size() == 1);
    CHECK(document == original);
}

TEST_CASE("Derived playback refuses a playing transport")
{
    Fixture fixture(48000.0, true);
    auto& transport = fixture.edit->getTransport();
    transport.ensureContextAllocated(true);
    transport.play(false);
    const bool playing = transport.isPlaying();
    CHECK_THROWS_AS(derivePlayback(ProjectDocument {}, fixture.track(), 48000.0), std::invalid_argument);
    transport.stop(false, false);
    transport.freePlaybackContext();
    REQUIRE(playing);
    CHECK(fixture.track().getClips().isEmpty());
}

TEST_CASE("Derived playback supplies extent for zero-duration events but no synthetic sounding note")
{
    Fixture fixture;
    ProjectDocument empty;
    CHECK(derivePlayback(empty, fixture.track(), 48000.0).clips.empty());
    empty.durationSeconds = 2.0;
    CHECK(derivePlayback(empty, fixture.track(), 48000.0).clips.empty());
    ProjectDocument document;
    document.events = { { 0.0, { 0xb0, 7, 98 } },
                        { 0.0, { 0x90, 60, 91 } }, { 0.0, { 0x80, 60, 73 } } };
    const auto original = document;
    const auto converted = derivePlayback(document, fixture.track(), 48000.0);
    REQUIRE(converted.clips.size() == 1);
    CHECK(converted.clips[0]->getPosition().getEnd().inSeconds() == 1.0 / 48000.0);
    CHECK(converted.suppressedShortNotes == 1);
    const std::vector<MidiEvent> expected { { 0.0, { 0xb0, 7, 98 } } };
    CHECK(exportEvents(converted) == expected);
    CHECK(playEvents(document, 48000.0) == expected);
    CHECK(document == original);
}

TEST_CASE("Derived playback keeps inclusive endpoint controllers at different rates")
{
    ProjectDocument document;
    document.durationSeconds = 1.0;
    document.events = { { 0.0, { 0xb2, 7, 100 } }, { 0.5, { 0xd2, 53 } }, { 1.0, { 0xc2, 22 } } };
    for (const double rate : { 48000.0, 44100.0 })
    {
        CAPTURE(rate);
        CHECK(playEvents(document, rate) == document.events);
    }
}

TEST_CASE("The pinned graph can advance a terminal note-off despite one-sample clip padding")
{
    ProjectDocument document;
    document.durationSeconds = 60.0;
    document.events = { { 59.0, { 0x92, 69, 100 } },
                        { 60.0, { 0x82, 69, 73 } }, { 60.0, { 0xb2, 1, 40 } } };
    const auto original = document;
    const auto played = playEvents(document, 48000.0);
    REQUIRE(played.size() == 3);
    CHECK(hasEvent(played, 59.0, { 0x92, 69, 100 }));
    CHECK(hasEvent(played, 60.0 - 1.0 / 48000.0, { 0x82, 69, 73 }));
    CHECK(hasEvent(played, 60.0, { 0xb2, 1, 40 }));
    CHECK(document == original);
}
