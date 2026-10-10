#include "InstrumentAdapter.h"
#include "AllocationProbe.h"

#include <composer/engine/EngineSetup.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <initializer_list>
#include <limits>

namespace
{
using Adapter = composer::app::InstrumentAdapter;
using Bridge = composer::app::RenderBridge;
using Fault = composer::app::BridgeFault;
using Processor = composer::instrument::InstrumentProcessor;

class ScratchDirectory
{
public:
    ScratchDirectory() : directory(juce::File::createTempFile("composer-adapter"))
    {
        REQUIRE(directory.createDirectory());
    }
    ~ScratchDirectory() { directory.deleteRecursively(); }
    juce::File directory;
};

struct Fixture
{
    ScratchDirectory scratch;
    std::unique_ptr<tracktion::Engine> engine =
        composer::engine::createHeadlessEngine("ComposerAdapterTests", scratch.directory);
    std::unique_ptr<tracktion::Edit> edit;
    tracktion::Plugin::Ptr plugin;
    Adapter* adapter = nullptr;

    Fixture()
    {
        engine->getPluginManager().createBuiltInType<Adapter>();
        edit = tracktion::Edit::createSingleTrackEdit(*engine);
        plugin = edit->getPluginCache().createNewPlugin(Adapter::xmlTypeName, {});
        adapter = dynamic_cast<Adapter*>(plugin.get());
        REQUIRE(adapter != nullptr);
        tracktion::getAudioTracks(*edit)[0]->pluginList.insertPlugin(plugin, 0, nullptr);
    }
};

struct Event
{
    int sample;
    std::array<std::uint8_t, 3> bytes;
    int size = 3;
};

tracktion::PluginRenderContext contextFor(juce::AudioBuffer<float>* audio, int start, int count,
                                        tracktion::MidiMessageArray* midi, double rate,
                                        double offset = 0.0)
{
    return { audio, start, count, midi, offset,
             { tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(count / rate) },
             true, false, false, false };
}

void fill(juce::AudioBuffer<float>& audio, float value)
{
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        std::fill_n(audio.getWritePointer(channel), audio.getNumSamples(), value);
}

struct Comparison
{
    Adapter& adapter;
    Processor direct;
    double rate = 48000.0;

    void prepare(double sampleRate)
    {
        rate = sampleRate;
        adapter.baseClassInitialise({ tracktion::TimePosition(), rate, Bridge::sliceCapacity });
        direct.setRateAndBufferSizeDetails(rate, Bridge::sliceCapacity);
        direct.prepareToPlay(rate, Bridge::sliceCapacity);
    }

    void release()
    {
        adapter.baseClassDeinitialise();
        direct.releaseResources();
    }

    float step(int count, std::initializer_list<Event> events = {}, double timestampOffset = 0.0,
               bool allNotesOff = false, bool nullAudio = false, int channels = 3)
    {
        constexpr int prefix = 11, suffix = 7;
        tracktion::MidiMessageArray midi;
        for (const auto& event : events)
            midi.addMidiMessage(juce::MidiMessage(event.bytes.data(), event.size),
                                event.sample / rate - timestampOffset, {});
        midi.isAllNotesOff = allNotesOff;
        juce::AudioBuffer<float> destination(channels, prefix + count + suffix);
        fill(destination, 7.0f);
        const auto context = contextFor(nullAudio ? nullptr : &destination, prefix, count,
                                        &midi, rate, timestampOffset);
        juce::AudioBuffer<float> expected(2, count);
        expected.clear();
        std::array<std::array<float, Bridge::sliceCapacity>, 2> storage {};
        std::array<float*, 2> pointers { storage[0].data(), storage[1].data() };
        juce::AudioBuffer<float> directAudio(pointers.data(), 2, Bridge::sliceCapacity);
        juce::MidiBuffer directMidi;
        directMidi.ensureSize(Bridge::eventCapacity * Bridge::midiEventStorageBytes);
        if (allNotesOff)
            direct.panic();
        for (int offset = 0; offset < count;)
        {
            const auto slice = std::min(Bridge::sliceCapacity, count - offset);
            directAudio.setDataToReferTo(pointers.data(), 2, slice);
            directAudio.clear();
            directMidi.clear();
            for (const auto& event : events)
            {
                const auto sample = std::max(0, event.sample);
                if (sample >= offset && sample < offset + slice)
                    directMidi.addEvent(event.bytes.data(), event.size, sample - offset);
            }
            direct.processBlock(directAudio, directMidi);
            for (int channel = 0; channel < 2; ++channel)
                expected.copyFrom(channel, offset, directAudio, channel, 0, slice);
            offset += slice;
        }

        adapter.applyToBuffer(context);
        CHECK(adapter.consumeRenderFaults() == 0);
        REQUIRE(midi.size() == static_cast<int>(events.size()));
        CHECK(midi.isAllNotesOff == allNotesOff);
        int index = 0;
        for (const auto& event : events)
        {
            const auto& actual = midi[index++];
            CHECK(actual.getTimeStamp() == event.sample / rate - timestampOffset);
            REQUIRE(actual.getRawDataSize() == event.size);
            CHECK(std::equal(actual.getRawData(), actual.getRawData() + event.size, event.bytes.begin()));
        }
        float peak = 0.0f;
        bool exact = true;
        if (!nullAudio)
            for (int channel = 0; channel < channels; ++channel)
                for (int sample = 0; sample < destination.getNumSamples(); ++sample)
                {
                    float wanted = 7.0f;
                    if (sample >= prefix && sample < prefix + count)
                    {
                        wanted = channel < 2 ? expected.getSample(channel, sample - prefix) : 0.0f;
                        if (channels == 1)
                            wanted = 0.5f * (expected.getSample(0, sample - prefix)
                                             + expected.getSample(1, sample - prefix));
                        peak = std::max(peak, std::abs(wanted));
                    }
                    exact = exact && std::isfinite(destination.getSample(channel, sample))
                                  && destination.getSample(channel, sample) == wanted;
                }
        CHECK(exact);
        return peak;
    }
};
}

TEST_CASE("The application instrument preserves sliced MIDI and shared processor samples")
{
    Fixture fixture;
    Comparison comparison { *fixture.adapter };
    const composer::contracts::InstrumentPatch patch {
        composer::contracts::Waveform::saw, -18.25, 0.0025, 0.07123456789,
        0.6180339887498948, 0.0333333333333333, 7654.321, 1.23456789012345 };
    REQUIRE(!fixture.adapter->applyPatch(patch));
    REQUIRE(!comparison.direct.applyPatch(patch));
    CHECK(fixture.adapter->currentPatch() == patch);
    auto invalid = patch;
    invalid.gainDb = 1.0;
    REQUIRE(fixture.adapter->applyPatch(invalid).has_value());
    CHECK(fixture.adapter->currentPatch() == patch);
    comparison.prepare(48000.0);
    CHECK(fixture.adapter->currentPatch() == patch);
    CHECK(fixture.adapter->getTailLength() == comparison.direct.getTailLengthSeconds());
    CHECK(comparison.step(3073, {
        { -4, { 0x80, 60, 17 } }, { 17, { 0x90, 60, 100 } }, { 91, { 0x91, 64, 87 } },
        { 1023, { 0x80, 60, 41 } }, { 1024, { 0xb0, 120, 0 } }, { 1024, { 0x90, 72, 99 } },
        { 2047, { 0x80, 72, 52 } }, { 2048, { 0x92, 55, 88 } }, { 3072, { 0x82, 55, 63 } },
        { 3073, { 0x90, 80, 100 } } }, 0.03125) > 0.001f);
    comparison.step(512, { { 64, { 0x90, 67, 96 } } }, 0.125, false, true);
    CHECK(comparison.step(257, {}, 0.0, false, false, 1) > 0.001f);
    comparison.step(128, {
        { 91, { 0x80, 67, 32 } }, { 17, { 0x90, 69, 80 } },
        { 17, { 0xc0, 4, 0 }, 2 }, { -1, { 0xd0, 5, 0 }, 2 },
        { 17, { 0x80, 69, 63 } }, { 0, { 0xb0, 1, 3 } } });
    comparison.release();
    CHECK(fixture.adapter->currentPatch() == patch);
    comparison.prepare(44100.0);
    CHECK(comparison.step(1025, { { 0, { 0x90, 60, 100 } }, { 1024, { 0x80, 60, 10 } } }) > 0.001f);
    comparison.release();
}

TEST_CASE("The application instrument distinguishes reset from a short panic fade")
{
    Fixture fixture;
    Comparison comparison { *fixture.adapter };
    comparison.prepare(48000.0);
    CHECK(comparison.step(2048, { { 0, { 0x90, 60, 100 } } }) > 0.001f);
    fixture.adapter->reset();
    comparison.direct.reset();
    CHECK(comparison.step(128) == 0.0f);
    comparison.step(2048, { { 0, { 0x90, 64, 100 } } });
    fixture.adapter->midiPanic();
    comparison.direct.panic();
    CHECK(comparison.step(128) > 0.0f);
    comparison.step(128);
    CHECK(comparison.step(128) == 0.0f);
    comparison.step(2048, { { 0, { 0x90, 67, 100 } } });
    comparison.step(0, {}, 0.0, true, true);
    CHECK(comparison.step(128) > 0.0f);
    comparison.step(128);
    CHECK(comparison.step(128) == 0.0f);
    comparison.release();
}

TEST_CASE("An invalid application render resets voices without touching unrelated samples")
{
    Fixture fixture;
    Comparison comparison { *fixture.adapter };
    comparison.prepare(48000.0);
    comparison.step(2048, { { 0, { 0x90, 60, 100 } } });
    constexpr int prefix = 9, count = 128, suffix = 5;
    tracktion::MidiMessageArray overflow;
    for (std::size_t index = 0; index <= Bridge::eventCapacity; ++index)
        overflow.addMidiMessage(juce::MidiMessage::controllerEvent(1, 7, 64), 0.0, {});
    juce::AudioBuffer<float> audio(2, prefix + count + suffix);
    fill(audio, 7.0f);
    fixture.adapter->applyToBuffer(contextFor(&audio, prefix, count, &overflow, comparison.rate));
    comparison.direct.reset();
    CHECK(fixture.adapter->consumeRenderFaults() == static_cast<unsigned>(Fault::capacityExceeded));
    CHECK(overflow.size() == static_cast<int>(Bridge::eventCapacity + 1));
    bool exact = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            exact = exact && audio.getSample(channel, sample)
                                == (sample >= prefix && sample < prefix + count ? 0.0f : 7.0f);
    CHECK(exact);
    CHECK(comparison.step(128) == 0.0f);
    CHECK(comparison.step(1025, { { 17, { 0x90, 69, 100 } } }) > 0.0f);
    fill(audio, 7.0f);
    fixture.adapter->applyToBuffer(contextFor(&audio, audio.getNumSamples(), 1, nullptr, comparison.rate));
    comparison.direct.reset();
    CHECK(fixture.adapter->consumeRenderFaults() == static_cast<unsigned>(Fault::invalidContext));
    bool untouched = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            untouched = untouched && audio.getSample(channel, sample) == 7.0f;
    CHECK(untouched);
    CHECK(comparison.step(128) == 0.0f);
    comparison.release();
}

TEST_CASE("Invalid application MIDI preserves input and resets held voices")
{
    Fixture fixture;
    Comparison comparison { *fixture.adapter };
    comparison.prepare(48000.0);
    struct InvalidEvent
    {
        std::array<std::uint8_t, 3> bytes;
        double timestamp;
    };
    const std::array<InvalidEvent, 5> invalidEvents {{
        { { 0x90, 0x80, 100 }, 0.0 },
        { { 0x90, 60, 0x80 }, 0.0 },
        { { 0x90, 60, 100 }, std::numeric_limits<double>::quiet_NaN() },
        { { 0x90, 60, 100 }, std::numeric_limits<double>::infinity() },
        { { 0x90, 60, 100 }, -std::numeric_limits<double>::infinity() }
    }};
    constexpr int prefix = 9, count = 128, suffix = 5;
    juce::AudioBuffer<float> audio(3, prefix + count + suffix);
    for (std::size_t index = 0; index < invalidEvents.size(); ++index)
    {
        CAPTURE(index);
        CHECK(comparison.step(2048, { { 0, { 0x90, 60, 100 } } }) > 0.001f);
        const auto& event = invalidEvents[index];
        tracktion::MidiMessageArray midi;
        midi.addMidiMessage(juce::MidiMessage(event.bytes.data(), static_cast<int>(event.bytes.size())),
                            event.timestamp, {});
        const auto originalTimestamp = std::bit_cast<std::uint64_t>(midi[0].getTimeStamp());
        fill(audio, 7.0f);
        fixture.adapter->applyToBuffer(contextFor(&audio, prefix, count, &midi, comparison.rate));
        CHECK(fixture.adapter->consumeRenderFaults() == static_cast<unsigned>(Fault::invalidMidi));
        CHECK(fixture.adapter->consumeRenderFaults() == 0);
        REQUIRE(midi.size() == 1);
        CHECK_FALSE(midi.isAllNotesOff);
        REQUIRE(midi[0].getRawDataSize() == static_cast<int>(event.bytes.size()));
        CHECK(std::equal(midi[0].getRawData(), midi[0].getRawData() + event.bytes.size(), event.bytes.begin()));
        CHECK(std::bit_cast<std::uint64_t>(midi[0].getTimeStamp()) == originalTimestamp);
        bool exact = true;
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                exact = exact && audio.getSample(channel, sample)
                                    == (sample >= prefix && sample < prefix + count ? 0.0f : 7.0f);
        CHECK(exact);
        comparison.direct.reset();
        CHECK(comparison.step(128) == 0.0f);
    }
    comparison.release();
}

TEST_CASE("Tracktion renders a clip through the application's shared instrument")
{
    Fixture fixture;
    auto patch = composer::contracts::InstrumentPatch {};
    patch.waveform = composer::contracts::Waveform::sine;
    patch.gainDb = -12.0;
    patch.releaseSeconds = 0.05;
    REQUIRE(!fixture.adapter->applyPatch(patch));
    auto& track = *tracktion::getAudioTracks(*fixture.edit)[0];
    auto clip = track.insertMIDIClip({ tracktion::TimePosition(),
                                      tracktion::TimePosition::fromSeconds(0.5) }, nullptr);
    REQUIRE(clip != nullptr);
    clip->getSequence().addNote(69, tracktion::BeatPosition(),
                                tracktion::BeatDuration::fromBeats(0.5), 100, 0, nullptr);
    const auto output = fixture.scratch.directory.getChildFile("render.wav");
    REQUIRE(tracktion::Renderer::renderToFile(*fixture.edit, output, false));
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(output));
    REQUIRE(reader != nullptr);
    REQUIRE(reader->numChannels == 2);
    REQUIRE(reader->lengthInSamples > 0);
    REQUIRE(reader->lengthInSamples <= std::numeric_limits<int>::max());
    juce::AudioBuffer<float> audio(2, static_cast<int>(reader->lengthInSamples));
    REQUIRE(reader->read(&audio, 0, audio.getNumSamples(), 0, true, true));
    bool finite = true;
    float peak = 0.0f;
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int sample = 0; sample < audio.getNumSamples(); ++sample)
        {
            finite = finite && std::isfinite(audio.getSample(channel, sample));
            peak = std::max(peak, std::abs(audio.getSample(channel, sample)));
        }
    CHECK(finite);
    CHECK(peak > 0.001f);
    CHECK(peak <= 1.0f);
    CHECK(fixture.adapter->currentPatch() == patch);
    CHECK(fixture.adapter->consumeRenderFaults() == 0);
}

TEST_CASE("Application instrument rendering retains preallocated storage at its MIDI limit")
{
    using Probe = composer::tests::AllocationProbe;
    if (!Probe::available())
        SKIP("Allocation observation requires the Windows debug runtime");

    // Confirm the hook sees an actual allocation and free on this thread.
    Probe::arm();
    void* (* volatile allocate)(std::size_t) = &std::malloc;
    void* memory = allocate(17);
    std::free(memory);
    const auto positiveControl = Probe::disarm();
    REQUIRE(positiveControl >= 2);

    Fixture fixture;
    fixture.adapter->baseClassInitialise({ tracktion::TimePosition(), 48000.0, Bridge::sliceCapacity });
    juce::AudioBuffer<float> audio(2, 2049);
    tracktion::MidiMessageArray midi;
    for (std::size_t index = 0; index < Bridge::eventCapacity; ++index)
        midi.addMidiMessage(juce::MidiMessage::controllerEvent(1, 7, 64), 0.0, {});
    const auto context = contextFor(&audio, 0, audio.getNumSamples(), &midi, 48000.0);
    for (int repetition = 0; repetition < 2; ++repetition)
    {
        Probe::arm();
        fixture.adapter->applyToBuffer(context);
        const auto operations = Probe::disarm();
        CHECK(operations == 0);
        CHECK(fixture.adapter->consumeRenderFaults() == 0);
    }
    CHECK(midi.size() == static_cast<int>(Bridge::eventCapacity));
    midi.addMidiMessage(juce::MidiMessage::noteOn(1, 60, 0.8f), 0.0, {});
    Probe::arm();
    fixture.adapter->applyToBuffer(context);
    const auto overflowOperations = Probe::disarm();
    CHECK(overflowOperations == 0);
    CHECK(fixture.adapter->consumeRenderFaults() == static_cast<unsigned>(Fault::capacityExceeded));
    fixture.adapter->baseClassDeinitialise();
    Probe::arm();
    fixture.adapter->applyToBuffer(context);
    const auto unpreparedOperations = Probe::disarm();
    CHECK(unpreparedOperations == 0);
    CHECK(fixture.adapter->consumeRenderFaults() == static_cast<unsigned>(Fault::notPrepared));
}

TEST_CASE("The application MIDI bridge preserves mixed message sizes and equal-sample order")
{
    Bridge bridge;
    bridge.prepare(48000.0);
    tracktion::MidiMessageArray source;
    const std::array<Event, 7> input {{
        { 127, { 0x90, 60, 99 } }, { 0, { 0xc0, 7, 0 }, 2 },
        { -2, { 0xd0, 23, 0 }, 2 }, { 0, { 0x80, 60, 42 } },
        { 1024, { 0xe0, 12, 65 } }, { 127, { 0xb0, 64, 127 } },
        { 1023, { 0xc0, 9, 0 }, 2 }
    }};
    for (const auto& event : input)
        source.addMidiMessage(juce::MidiMessage(event.bytes.data(), event.size), event.sample / 48000.0, {});
    const auto context = contextFor(nullptr, 0, 1025, &source, 48000.0);
    REQUIRE(bridge.begin(context) == Fault::none);
    for (const int offset : { 0, 1024 })
    {
        const auto count = offset == 0 ? 1024 : 1;
        juce::MidiBuffer expected;
        for (const auto& event : input)
        {
            const auto sample = std::max(0, event.sample);
            if (sample >= offset && sample < offset + count)
                REQUIRE(expected.addEvent(event.bytes.data(), event.size, sample - offset));
        }
        bridge.prepareSlice(offset, count);
        const auto& actual = bridge.midi();
        REQUIRE(actual.data.size() == expected.data.size());
        CHECK(std::equal(actual.data.begin(), actual.data.end(), expected.data.begin()));
        CHECK(actual.getNumEvents() == expected.getNumEvents());
    }
    CHECK(source.size() == static_cast<int>(input.size()));
}
