#include "CaptureClock.h"
#include "RecordingCapture.h"

#include <composer/engine/EngineSetup.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
using composer::app::CaptureCalibration;
using composer::app::CaptureClock;
using composer::app::CaptureClockSnapshot;
using composer::app::CaptureClockWitness;
using composer::app::CaptureInterruption;
using composer::app::CaptureMapping;
using composer::app::RecordingCapture;
constexpr int blockSize = 128;

struct Bytes
{
    std::array<std::uint8_t, 3> value{};
    int size = 3;
    bool operator==(const Bytes&) const = default;
};

class MonitorObserver final : public tracktion::Plugin
{
public:
    static constexpr const char* xmlTypeName = "composer_test_capture_monitor";
    static const char* getPluginName() { return "Capture Monitor Test"; }
    explicit MonitorObserver(tracktion::PluginCreationInfo info) : Plugin(info) {}
    ~MonitorObserver() override { notifyListenersOfDeletion(); }
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override { return true; }
    bool takesAudioInput() override { return true; }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    void initialise(const tracktion::PluginInitialisationInfo&) override {}
    void deinitialise() override {}
    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        if (!context.isPlaying || context.bufferForMidiMessages == nullptr) return;
        for (const auto& message : *context.bufferForMidiMessages)
        {
            const auto size = message.getRawDataSize();
            if (size < 1 || size > 3 || count >= messages.size())
            {
                overflow = true;
                continue;
            }
            auto& result = messages[count++];
            result.size = size;
            std::copy_n(message.getRawData(), size, result.value.begin());
        }
    }
    std::array<Bytes, 512> messages{};
    std::size_t count = 0;
    bool overflow = false;
};

struct ScratchDirectory
{
    ScratchDirectory() : directory(juce::File::createTempFile("composer-capture"))
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
        composer::engine::createHeadlessEngine("ComposerCaptureIntegrationTests", scratch.directory);
    tracktion::DeviceManager& devices = engine->getDeviceManager();
    tracktion::HostedAudioDeviceInterface& hosted = devices.getHostedAudioDeviceInterface();
    std::shared_ptr<tracktion::MidiInputDevice> input;
    std::unique_ptr<tracktion::Edit> edit;
    CaptureClock* clock = nullptr;
    MonitorObserver* observer = nullptr;
    juce::AudioBuffer<float> audio{2, blockSize};
    juce::MidiBuffer midi;

    Fixture()
    {
        engine->getPluginManager().createBuiltInType<CaptureClockWitness>();
        engine->getPluginManager().createBuiltInType<MonitorObserver>();
        tracktion::HostedAudioDeviceInterface::Parameters parameters;
        parameters.sampleRate = 48000;
        parameters.blockSize = blockSize;
        parameters.useMidiDevices = false;
        parameters.inputChannels = 0;
        parameters.outputChannels = 2;
        hosted.initialise(parameters);
        hosted.prepareToPlay(48000, blockSize);
        devices.dispatchPendingUpdates();
        devices.rescanMidiDeviceList();
        pump();
        for (const auto& device : devices.getMidiInDevices())
        {
            REQUIRE(device->getDeviceType() != tracktion::InputDevice::physicalMidiDevice);
            if (dynamic_cast<tracktion::VirtualMidiInputDevice*>(device.get()) != nullptr)
            {
                REQUIRE(input == nullptr);
                input = device;
            }
        }
        REQUIRE(input != nullptr);
        input->setEnabled(true);
        input->setMonitorMode(tracktion::InputDevice::MonitorMode::on);
        edit = tracktion::createEmptyEdit(*engine, scratch.directory.getChildFile("capture.tracktionedit"));
        edit->ensureNumberOfAudioTracks(1);
        edit->tempoSequence.getTempo(0)->setBpm(120.0);
        edit->clickTrackEnabled = false;
        edit->playInStopEnabled = true;
        auto* track = tracktion::getAudioTracks(*edit)[0];
        auto observation = edit->getPluginCache().createNewPlugin(MonitorObserver::xmlTypeName, {});
        observer = dynamic_cast<MonitorObserver*>(observation.get());
        REQUIRE(observer != nullptr);
        track->pluginList.insertPlugin(observation, 0, nullptr);
        auto plugin = edit->getPluginCache().createNewPlugin(CaptureClockWitness::xmlTypeName, {});
        auto* witness = dynamic_cast<CaptureClockWitness*>(plugin.get());
        REQUIRE(witness != nullptr);
        track->pluginList.insertPlugin(plugin, 1, nullptr);
        transport().ensureContextAllocated(true);
        bool targeted = false;
        for (auto* instance : edit->getAllInputDevices())
            if (&instance->getInputDevice() == input.get())
            {
                REQUIRE(static_cast<bool>(instance->setTarget(track->itemID, true, &edit->getUndoManager(), 0)));
                targeted = true;
            }
        REQUIRE(targeted);
        edit->restartPlayback();
        devices.dispatchPendingUpdates();
        pump();
        REQUIRE(devices.getGlobalOutputAudioProcessor() == nullptr);
        auto publisher = std::make_unique<CaptureClock>(devices, input, *witness);
        clock = publisher.get();
        devices.setGlobalOutputAudioProcessor(std::move(publisher));
    }

    ~Fixture()
    {
        devices.setGlobalOutputAudioProcessor(nullptr);
        if (edit)
        {
            transport().stop(false, false);
            transport().freePlaybackContext();
        }
    }

    static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); }
    tracktion::TransportControl& transport() { return edit->getTransport(); }
    CaptureClockSnapshot snapshot()
    {
        const auto result = clock->read();
        REQUIRE(result.has_value());
        return *result;
    }
    CaptureClockSnapshot step()
    {
        audio.clear();
        midi.clear();
        hosted.processBlock(audio, midi);
        return snapshot();
    }
    CaptureMapping prepare()
    {
        step();
        const auto generation = clock->beginGeneration();
        REQUIRE(generation.has_value());
        transport().setPosition(tracktion::TimePosition());
        transport().play(false);
        CaptureCalibration calibration(*generation);
        step();
        REQUIRE_FALSE(calibration.poll(*clock, transport().getCurrentPlaybackContext()));
        const auto second = step();
        REQUIRE_FALSE(calibration.poll(*clock, transport().getCurrentPlaybackContext()));
        CaptureClockSnapshot complete{};
        std::optional<CaptureMapping> mapping;
        for (int attempt = 0; attempt < 32 && !mapping; ++attempt)
        {
            complete = step();
            mapping = calibration.poll(*clock, transport().getCurrentPlaybackContext());
        }
        REQUIRE(mapping.has_value());
        REQUIRE(mapping->proofSerial == complete.serial);
        REQUIRE(mapping->generation == complete.generation);
        REQUIRE(mapping->takeStartEditSample == second.graphEditEnd);
        return *mapping;
    }

    std::int64_t deliver(RecordingCapture& capture, const CaptureMapping& mapping,
                         const Bytes& bytes, int offset)
    {
        const auto published = snapshot();
        REQUIRE(published.valid);
        REQUIRE(published.generation == mapping.generation);
        REQUIRE(clock->signals().correction.load() == input->getAdjustSecs());
        const auto streamSample = published.streamEnd + offset;
        const auto timestamp = static_cast<double>(streamSample) / mapping.sampleRate
                             - clock->signals().correction.load();
        REQUIRE(timestamp >= 0.0);
        const juce::MidiMessage message(bytes.value.data(), bytes.size, timestamp);
        // Both calls finish synchronously. No physical callback is registered here.
        capture.handleIncomingMidiMessage(nullptr, message);
        input->handleIncomingMidiMessage(message, input->getMPESourceID());
        return streamSample + mapping.streamToEditOffset - mapping.takeStartEditSample;
    }
};
}

TEST_CASE("A calibrated publisher feeds lossless raw capture and one virtual monitor delivery")
{
    Fixture fixture;
    const auto mapping = fixture.prepare();
    REQUIRE(fixture.observer->count == 0);
    RecordingCapture capture(fixture.clock->signals(), mapping, 192, 1000, 100);
    REQUIRE(capture.open(1000));
    struct Expected
    {
        Bytes bytes;
        std::int64_t relativeSample;
    };
    std::vector<Expected> expected;
    expected.reserve(144);
    const auto submit = [&](Bytes bytes, int offset)
    {
        expected.push_back({bytes, fixture.deliver(capture, mapping, bytes, offset)});
    };
    for (int channel = 0; channel < 16; ++channel)
    {
        const auto status = [channel](int kind) { return static_cast<std::uint8_t>(kind | channel); };
        const auto pitch = static_cast<std::uint8_t>(60 + channel);
        submit({{status(0x90), pitch, 97}}, 17);
        submit({{status(0x90), pitch, 83}}, 17); // Preserve a repeated pitch without closing the first on.
        submit({{status(0x80), 111, 41}}, 91); // Preserve an unmatched note-off.
        submit({{status(0xa0), pitch, 73}}, 33);
        submit({{status(0xb0), 1, 64}}, 91);
        submit({{status(0xc0), 19, 0}, 2}, 91);
        submit({{status(0xd0), 55, 0}, 2}, 33);
        submit({{status(0xe0), 12, 65}}, 109);
        submit({{status(0x80), pitch, 37}}, 109);
    }
    fixture.step();
    REQUIRE(capture.poll(1010));
    const auto completed = fixture.step();
    REQUIRE(capture.poll(1020));
    REQUIRE_FALSE(fixture.observer->overflow);
    REQUIRE(fixture.observer->count == expected.size());
    for (const auto& event : expected)
        CHECK(std::count(fixture.observer->messages.begin(),
                         fixture.observer->messages.begin() + fixture.observer->count, event.bytes) == 1);

    // All explicit producers have returned. Closing is followed by this proven
    // quiescence boundary before sealing; no registry or device join is simulated.
    capture.close();
    CHECK_FALSE(capture.accepting());
    capture.sealAfterJoin(1020);
    fixture.clock->invalidate();
    fixture.transport().stop(false, false);
    const auto& take = capture.collect();
    REQUIRE(take.isComplete());
    CHECK(take.interruptionFaults == 0);
    CHECK(take.captured.invalidTimestamps == 0);
    CHECK(take.captured.invalidMessages == 0);
    CHECK(take.captured.overflowEvents == 0);
    REQUIRE(take.captured.events.size() == expected.size());
    std::size_t adjusted = 0;
    std::int64_t previous = 0;
    for (auto& event : expected)
    {
        if (event.relativeSample < previous)
        {
            event.relativeSample = previous;
            ++adjusted;
        }
        else previous = event.relativeSample;
    }
    CHECK(adjusted == 107);
    CHECK(take.arrivalTimeAdjustments == adjusted);
    CHECK(take.preOriginEvents == 0);
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        CAPTURE(index);
        const auto& wanted = expected[index];
        const auto& actual = take.captured.events[index];
        CHECK(actual.timeSeconds == static_cast<double>(wanted.relativeSample) / mapping.sampleRate);
        REQUIRE(actual.bytes.size() == static_cast<std::size_t>(wanted.bytes.size));
        CHECK(std::equal(actual.bytes.begin(), actual.bytes.end(), wanted.bytes.value.begin()));
    }
    const auto mappedCompletedEnd = completed.streamEnd + mapping.streamToEditOffset - mapping.takeStartEditSample;
    INFO("Completed graph endpoint: " << completed.graphEditEnd << "; mapped raw endpoint: " << mappedCompletedEnd);
    CHECK(mappedCompletedEnd >= expected.back().relativeSample);
    CHECK(&take == &capture.collect());
    CHECK_FALSE(capture.open(1030));
}

TEST_CASE("The first current-time MIDI event is admitted immediately after calibration")
{
    bool wholeMilliseconds = false;
    SECTION("High-resolution current time") {}
    SECTION("Whole-millisecond current time") { wholeMilliseconds = true; }
    Fixture fixture;
    const auto mapping = fixture.prepare();
    RecordingCapture capture(fixture.clock->signals(), mapping, 4, 1000, 100);
    REQUIRE(capture.open(1000));
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto timestamp = (wholeMilliseconds ? std::floor(now) : now) * 0.001;
    const std::array<std::uint8_t, 3> bytes{0x92, 67, 100};
    const juce::MidiMessage message(bytes.data(), static_cast<int>(bytes.size()), timestamp);
    // No extra hosted block occurs between calibration and the first callback.
    capture.handleIncomingMidiMessage(nullptr, message);
    capture.close();
    capture.sealAfterJoin(1000);
    const auto& take = capture.collect();
    CHECK(take.isComplete());
    CHECK(take.captured.invalidTimestamps == 0);
    CHECK(take.preOriginEvents == 0);
    CHECK(take.arrivalTimeAdjustments == 0);
    REQUIRE(take.captured.events.size() == 1);
    const auto& event = take.captured.events.front();
    CHECK(std::isfinite(event.timeSeconds));
    CHECK(event.timeSeconds >= 0.0);
    CHECK(event.bytes == std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

TEST_CASE("A real publisher interruption freezes an incomplete take without further MIDI")
{
    Fixture fixture;
    const auto mapping = fixture.prepare();
    RecordingCapture capture(fixture.clock->signals(), mapping, 4, 1000, 100);
    REQUIRE(capture.open(1000));
    const Bytes bytes{{0x9f, 67, 93}};
    const auto relativeSample = fixture.deliver(capture, mapping, bytes, 17);
    fixture.step();
    REQUIRE(capture.poll(1010));
    SECTION("A hosted sample-rate change invalidates the recording generation")
    {
        fixture.hosted.prepareToPlay(44100, blockSize);
        REQUIRE(fixture.snapshot().generation > mapping.generation);
        REQUIRE(fixture.snapshot().rate == 44100.0);
        fixture.devices.dispatchPendingUpdates();
        Fixture::pump();
    }
    SECTION("A repeated stream endpoint latches a publisher fault")
    {
        fixture.clock->processBlock(fixture.audio, fixture.midi);
        REQUIRE(fixture.snapshot().generation > mapping.generation);
        REQUIRE_FALSE(fixture.snapshot().valid);
        fixture.step();
        fixture.step();
        REQUIRE_FALSE(fixture.snapshot().valid);
    }
    // No callback and no owner poll occurs after the interruption. The seal's
    // final actual-publisher check must prevent a falsely successful take.
    capture.close();
    capture.sealAfterJoin(1020);
    const auto& take = capture.collect();
    REQUIRE_FALSE(take.isComplete());
    CHECK((take.interruptionFaults & static_cast<unsigned>(CaptureInterruption::generationChanged)) != 0);
    CHECK((take.interruptionFaults & static_cast<unsigned>(CaptureInterruption::invalidCorrection)) != 0);
    REQUIRE(take.captured.events.size() == 1);
    CHECK(take.captured.events[0].timeSeconds == static_cast<double>(relativeSample) / mapping.sampleRate);
    CHECK(take.captured.events[0].bytes == std::vector<std::uint8_t>(bytes.value.begin(), bytes.value.end()));
    const auto frozenFaults = take.interruptionFaults;
    fixture.transport().stop(false, false);
    const auto nextMapping = fixture.prepare();
    REQUIRE(nextMapping.generation > mapping.generation);
    CHECK_FALSE(capture.open(1030));
    CHECK_FALSE(capture.poll(1030));
    CHECK_FALSE(capture.collect().isComplete());
    CHECK(capture.collect().interruptionFaults == frozenFaults);
    CHECK(&take == &capture.collect());
}
