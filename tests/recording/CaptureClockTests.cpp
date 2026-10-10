#include "CaptureClock.h"
#include "AllocationProbe.h"

#include <composer/engine/EngineSetup.h>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
using composer::app::CaptureCalibration;
using composer::app::CaptureClock;
using composer::app::CaptureClockSnapshot;
using composer::app::CaptureClockWitness;
using composer::app::CaptureMapping;
constexpr int blockSize = 128;

std::int64_t samples(double seconds, double rate)
{
    return static_cast<std::int64_t>(seconds * rate + 0.5);
}

struct ScratchDirectory
{
    ScratchDirectory() : directory(juce::File::createTempFile("composer-clock"))
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
        composer::engine::createHeadlessEngine("ComposerClockTests", scratch.directory);
    tracktion::DeviceManager& devices = engine->getDeviceManager();
    tracktion::HostedAudioDeviceInterface& hosted = devices.getHostedAudioDeviceInterface();
    std::shared_ptr<tracktion::MidiInputDevice> input;
    std::unique_ptr<tracktion::Edit> edit;
    tracktion::AudioTrack* track = nullptr;
    CaptureClockWitness* witness = nullptr;
    CaptureClock* clock = nullptr;
    std::unique_ptr<CaptureClock> uninstalledClock;
    juce::AudioBuffer<float> audio { 2, blockSize };
    juce::MidiBuffer midi;

    explicit Fixture(bool install = true)
    {
        engine->getPluginManager().createBuiltInType<CaptureClockWitness>();
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
        input->recordingEnabled = true; // The engine recorder supplies a timing oracle.
        input->setMonitorMode(tracktion::InputDevice::MonitorMode::on);
        edit = tracktion::createEmptyEdit(*engine, scratch.directory.getChildFile("clock.tracktionedit"));
        edit->ensureNumberOfAudioTracks(1);
        edit->tempoSequence.getTempo(0)->setBpm(120.0);
        edit->clickTrackEnabled = false;
        edit->playInStopEnabled = true;
        track = tracktion::getAudioTracks(*edit)[0];
        auto plugin = edit->getPluginCache().createNewPlugin(CaptureClockWitness::xmlTypeName, {});
        witness = dynamic_cast<CaptureClockWitness*>(plugin.get());
        REQUIRE(witness != nullptr);
        track->pluginList.insertPlugin(plugin, 0, nullptr);
        transport().ensureContextAllocated(true);
        bool targeted = false;
        for (auto* instance : edit->getAllInputDevices())
            if (&instance->getInputDevice() == input.get())
            {
                REQUIRE(static_cast<bool>(instance->setTarget(track->itemID, true, &edit->getUndoManager(), 0)));
                instance->setRecordingEnabled(track->itemID, true);
                targeted = true;
            }
        REQUIRE(targeted);
        edit->restartPlayback();
        devices.dispatchPendingUpdates();
        pump();
        REQUIRE(devices.getGlobalOutputAudioProcessor() == nullptr);
        uninstalledClock = std::make_unique<CaptureClock>(devices, input, *witness);
        clock = uninstalledClock.get();
        if (install)
            devices.setGlobalOutputAudioProcessor(std::move(uninstalledClock));
        else
            clock->prepareToPlay(48000, blockSize);
    }

    ~Fixture()
    {
        devices.setGlobalOutputAudioProcessor(nullptr);
        uninstalledClock.reset();
        if (edit)
        {
            transport().stop(false, false);
            transport().freePlaybackContext();
        }
    }

    static void pump() { juce::MessageManager::getInstance()->runDispatchLoopUntil(20); }
    tracktion::TransportControl& transport() { return edit->getTransport(); }
    tracktion::EditPlaybackContext* context() { return transport().getCurrentPlaybackContext(); }

    CaptureClockSnapshot snapshot()
    {
        const auto result = clock->read();
        REQUIRE(result.has_value());
        return *result;
    }

    void hostStep()
    {
        audio.clear();
        midi.clear();
        hosted.processBlock(audio, midi);
    }

    CaptureClockSnapshot step()
    {
        const auto before = snapshot();
        hostStep();
        const auto result = snapshot();
        CHECK(result.serial == before.serial + 1);
        // Sequential hosted processing makes this non-atomic engine clock a safe oracle.
        CHECK(result.streamEnd == samples(devices.getCurrentStreamTime(), result.rate));
        CHECK(result.correction == input->getAdjustSecs());
        CHECK(result.graphSerial == witness->serial.load());
        CHECK(result.graphEditEnd == witness->editEnd.load());
        CHECK(clock->signals().generation.load() == result.generation);
        CHECK(clock->signals().serial.load() == result.serial);
        if (result.valid)
            CHECK(clock->signals().correction.load() == result.correction);
        else
            CHECK(std::isnan(clock->signals().correction.load()));
        return result;
    }

    CaptureMapping calibrate(CaptureCalibration& calibration)
    {
        step();
        CHECK_FALSE(calibration.poll(*clock, context()));
        CHECK_FALSE(calibration.poll(*clock, context()));
        const auto second = step();
        CHECK_FALSE(calibration.poll(*clock, context()));
        CHECK_FALSE(calibration.poll(*clock, context()));
        CaptureClockSnapshot proving{};
        std::optional<CaptureMapping> result;
        for (int attempt = 0; attempt < 32 && !result; ++attempt)
        {
            proving = step();
            result = calibration.poll(*clock, context());
        }
        REQUIRE(result.has_value());
        CHECK(result->proofSerial == proving.serial);
        CHECK(result->proofSerial > second.serial);
        CHECK(result->takeStartEditSample == second.graphEditEnd);
        CHECK(result->generation == proving.generation);
        CHECK(result->sampleRate == proving.rate);
        CHECK(calibration.isContextCurrent(context()));
        return *result;
    }
};
}

TEST_CASE("Recording calibration requires two subsequently completed graph points")
{
    Fixture fixture;
    CaptureCalibration early(fixture.snapshot().generation);
    CHECK_FALSE(early.poll(*fixture.clock, fixture.context()));
    fixture.step();
    CHECK(std::isnan(fixture.clock->signals().correction.load()));
    const auto generation = fixture.clock->beginGeneration();
    REQUIRE(generation.has_value());
    fixture.step();
    CaptureCalibration calibration(*generation);
    CHECK_FALSE(calibration.poll(*fixture.clock, fixture.context()));
    fixture.transport().setPosition(tracktion::TimePosition());
    fixture.transport().play(false);
    CHECK_FALSE(calibration.poll(*fixture.clock, fixture.context()));
    fixture.calibrate(calibration);
    fixture.transport().stop(false, false);
    CHECK_FALSE(calibration.isContextCurrent(fixture.context()));
    CHECK_FALSE(calibration.poll(*fixture.clock, fixture.context()));

    fixture.transport().setLoopRange({ tracktion::TimePosition(), tracktion::TimePosition::fromSeconds(1.0) });
    fixture.transport().looping = true;
    fixture.transport().play(false);
    fixture.step();
    REQUIRE(fixture.context() != nullptr);
    REQUIRE(fixture.context()->isLooping());
    CHECK_FALSE(calibration.isContextCurrent(fixture.context()));
    CHECK_FALSE(calibration.poll(*fixture.clock, fixture.context()));
    CHECK_FALSE(calibration.poll(*fixture.clock, nullptr));
}

TEST_CASE("Frozen recording mapping agrees with the engine recorder at irregular event offsets")
{
    Fixture fixture;
    fixture.step();
    const auto generation = fixture.clock->beginGeneration();
    REQUIRE(generation.has_value());
    fixture.transport().setPosition(tracktion::TimePosition());
    fixture.transport().record(false, false);
    REQUIRE(fixture.transport().isRecording());
    CaptureCalibration calibration(*generation);
    const auto mapping = fixture.calibrate(calibration);
    struct Expected
    {
        int pitch, blockOffset;
        bool noteOn;
        std::int64_t predictedEdit = 0, recorderEdit = -1;
    };
    std::array<Expected, 4> expected {{ { 60, 17, true }, { 67, 91, true },
                                     { 60, 33, false }, { 67, 109, false } }};
    const auto inject = [&](Expected& event)
    {
        const auto clock = fixture.snapshot();
        REQUIRE(clock.valid);
        REQUIRE(clock.generation == mapping.generation);
        const auto correctedStream = clock.streamEnd + event.blockOffset;
        const auto timestamp = static_cast<double>(correctedStream) / clock.rate
                             - fixture.clock->signals().correction.load();
        const auto reconstructed = samples(timestamp + clock.correction, clock.rate);
        REQUIRE(reconstructed == correctedStream);
        event.predictedEdit = reconstructed + mapping.streamToEditOffset;
        REQUIRE(fixture.context() != nullptr);
        CHECK(samples(fixture.context()->globalStreamTimeToEditTimeUnlooped(timestamp + clock.correction).inSeconds(),
                      clock.rate) == event.predictedEdit);
        REQUIRE(event.predictedEdit >= mapping.takeStartEditSample);
        auto message = event.noteOn ? juce::MidiMessage::noteOn(1, event.pitch, static_cast<juce::uint8>(100))
                                    : juce::MidiMessage::noteOff(1, event.pitch, static_cast<juce::uint8>(41));
        message.setTimeStamp(timestamp);
        fixture.input->handleIncomingMidiMessage(message, fixture.input->getMPESourceID());
    };
    inject(expected[0]);
    inject(expected[1]);
    fixture.step();
    fixture.step();
    inject(expected[2]);
    inject(expected[3]);
    fixture.step();
    fixture.step();
    fixture.transport().stop(false, true);
    Fixture::pump();
    REQUIRE_FALSE(fixture.transport().isRecording());
    int recordedNotes = 0;
    for (auto* base : fixture.track->getClips())
        if (auto* clip = dynamic_cast<tracktion::MidiClip*>(base))
            for (const auto* note : clip->getSequence().getNotes())
            {
                const auto found = std::find_if(expected.begin(), expected.end(), [&](const auto& event)
                {
                    return event.noteOn && event.pitch == note->getNoteNumber();
                });
                REQUIRE(found != expected.end());
                const auto index = static_cast<std::size_t>(std::distance(expected.begin(), found));
                expected[index].recorderEdit = samples(note->getEditStartTime(*clip).inSeconds(), mapping.sampleRate);
                expected[index + 2].recorderEdit = samples(note->getEditEndTime(*clip).inSeconds(), mapping.sampleRate);
                ++recordedNotes;
            }
    REQUIRE(recordedNotes == 2);
    for (const auto& event : expected)
    {
        CAPTURE(event.pitch, event.noteOn, event.blockOffset);
        CHECK(event.recorderEdit == event.predictedEdit);
    }
}

TEST_CASE("Recording generations require the current enabled MIDI input")
{
    Fixture fixture;
    SECTION("A disabled current input cannot start a generation")
    {
        fixture.input->setEnabled(false);
        CHECK_FALSE(fixture.clock->beginGeneration());
        CHECK_FALSE(fixture.snapshot().valid);
        CHECK(std::isnan(fixture.clock->signals().correction.load()));
    }
    SECTION("An enabled input with the same identifier cannot impersonate the current input")
    {
        auto replacement = std::make_shared<tracktion::VirtualMidiInputDevice>(
            *fixture.engine, fixture.input->getName(), fixture.input->getDeviceType(),
            fixture.input->getDeviceID(), false);
        replacement->setEnabled(true);
        REQUIRE(replacement->isEnabled());
        REQUIRE(fixture.devices.findMidiInputDeviceForID(replacement->getDeviceID()).get() == fixture.input.get());
        CaptureClock stale(fixture.devices, replacement, *fixture.witness);
        stale.prepareToPlay(48000, blockSize);
        CHECK_FALSE(stale.beginGeneration());
        REQUIRE(stale.read());
        CHECK_FALSE(stale.read()->valid);
        CHECK(std::isnan(stale.signals().correction.load()));
    }
    SECTION("Disabling the active input latches a fault until a fresh generation")
    {
        fixture.step();
        const auto generation = fixture.clock->beginGeneration();
        REQUIRE(generation);
        fixture.transport().play(false);
        REQUIRE(fixture.step().valid);
        fixture.input->setEnabled(false);
        const auto disabled = fixture.step();
        CHECK_FALSE(disabled.valid);
        CHECK(disabled.generation > *generation);
        CHECK(std::isnan(fixture.clock->signals().correction.load()));
        fixture.input->setEnabled(true);
        const auto enabled = fixture.step();
        CHECK_FALSE(enabled.valid);
        CHECK(enabled.generation == disabled.generation);
        const auto recovered = fixture.clock->beginGeneration();
        REQUIRE(recovered);
        CHECK(*recovered > disabled.generation);
        CHECK(fixture.step().valid);
    }
}

TEST_CASE("A recording publisher retains its witness after removal from the stopped graph")
{
    Fixture fixture(false);
    fixture.transport().stop(false, false);
    fixture.transport().freePlaybackContext();
    // Keep a test reference so even a missing publisher reference is observable
    // without dereferencing a destroyed plugin.
    tracktion::Plugin::Ptr observed(fixture.witness);
    fixture.witness->deleteFromParent();
    const auto withPublisher = observed->getReferenceCount();
    fixture.uninstalledClock.reset();
    fixture.clock = nullptr;
    CHECK(observed->getReferenceCount() == withPublisher - 1);
}

TEST_CASE("Recording clocks latch stream faults and require fresh calibration after a real rate change")
{
    Fixture fixture;
    fixture.step();
    const auto firstGeneration = fixture.clock->beginGeneration();
    REQUIRE(firstGeneration.has_value());
    fixture.transport().play(false);
    CaptureCalibration original(*firstGeneration);
    fixture.calibrate(original);

    fixture.audio.clear();
    fixture.audio.setSample(0, 17, 0.375f);
    fixture.audio.setSample(1, 91, -0.625f);
    fixture.midi.addEvent(juce::MidiMessage::controllerEvent(3, 7, 64), 91);
    const juce::MidiBuffer originalMidi(fixture.midi);
    // The engine stream has not advanced: this directly injects a publisher fault.
    fixture.clock->processBlock(fixture.audio, fixture.midi);
    const auto fault = fixture.snapshot();
    CHECK_FALSE(fault.valid);
    CHECK(fault.generation > *firstGeneration);
    CHECK(std::isnan(fixture.clock->signals().correction.load()));
    CHECK(fixture.audio.getSample(0, 17) == 0.375f);
    CHECK(fixture.audio.getSample(1, 91) == -0.625f);
    CHECK(fixture.audio.getMagnitude(0, 0, blockSize) == 0.375f);
    CHECK(fixture.audio.getMagnitude(1, 0, blockSize) == 0.625f);
    REQUIRE(fixture.midi.data.size() == originalMidi.data.size());
    CHECK(std::equal(fixture.midi.data.begin(), fixture.midi.data.end(), originalMidi.data.begin()));
    fixture.step();
    fixture.step();
    CHECK_FALSE(fixture.snapshot().valid);
    CHECK(fixture.snapshot().generation == fault.generation);
    CHECK_FALSE(original.poll(*fixture.clock, fixture.context()));

    const auto recoveredGeneration = fixture.clock->beginGeneration();
    REQUIRE(recoveredGeneration.has_value());
    REQUIRE(*recoveredGeneration > fault.generation);
    CaptureCalibration recovered(*recoveredGeneration);
    fixture.calibrate(recovered);
    fixture.hosted.prepareToPlay(44100, blockSize);
    const auto rateChanged = fixture.snapshot();
    CHECK(rateChanged.generation > *recoveredGeneration);
    CHECK(rateChanged.rate == 44100.0);
    CHECK_FALSE(rateChanged.valid);
    CHECK(std::isnan(fixture.clock->signals().correction.load()));
    fixture.devices.dispatchPendingUpdates();
    Fixture::pump();
    fixture.step();
    fixture.step();
    CHECK_FALSE(recovered.poll(*fixture.clock, fixture.context()));
    CHECK_FALSE(fixture.snapshot().valid);
    const auto newGeneration = fixture.clock->beginGeneration();
    REQUIRE(newGeneration.has_value());
    CaptureCalibration newRate(*newGeneration);
    CHECK_FALSE(newRate.poll(*fixture.clock, fixture.context()));
    fixture.transport().stop(false, false);
    fixture.transport().setPosition(tracktion::TimePosition());
    fixture.transport().play(false);
    const auto newMapping = fixture.calibrate(newRate);
    CHECK(newMapping.sampleRate == 44100.0);
    CHECK(newMapping.generation > *recoveredGeneration);

    fixture.clock->releaseResources();
    const auto released = fixture.snapshot();
    CHECK(released.generation > newMapping.generation);
    CHECK_FALSE(fixture.clock->beginGeneration());
    fixture.step();
    CHECK_FALSE(fixture.snapshot().valid);
    fixture.clock->prepareToPlay(44100, blockSize);
    fixture.step();
    CHECK_FALSE(fixture.snapshot().valid);
    const auto preparedGeneration = fixture.clock->beginGeneration();
    REQUIRE(preparedGeneration.has_value());
    CaptureCalibration prepared(*preparedGeneration);
    fixture.calibrate(prepared);
    fixture.clock->invalidate();
    CHECK_FALSE(fixture.snapshot().valid);
    CHECK_FALSE(prepared.poll(*fixture.clock, fixture.context()));
}

TEST_CASE("The recording graph witness preserves every audio sample and MIDI byte")
{
    Fixture fixture;
    fixture.transport().freePlaybackContext();
    fixture.witness->baseClassInitialise({ tracktion::TimePosition(), 48000.0, blockSize });
    juce::AudioBuffer<float> audio(3, blockSize + 14);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int index = 0; index < audio.getNumSamples(); ++index)
            audio.setSample(channel, index, static_cast<float>(channel * 200 + index) / 1024.0f);
    const juce::AudioBuffer<float> originalAudio(audio);
    tracktion::MidiMessageArray midi;
    const std::array<std::uint8_t, 3> bytes { 0x92, 67, 113 };
    midi.addMidiMessage(juce::MidiMessage(bytes.data(), static_cast<int>(bytes.size())), 0.0012345, {});
    midi.isAllNotesOff = true;
    const auto serial = fixture.witness->serial.load();
    tracktion::PluginRenderContext context {
        &audio, 7, blockSize, &midi, 0.25,
        { tracktion::TimePosition::fromSeconds(1.0), tracktion::TimePosition::fromSeconds(1.0 + blockSize / 48000.0) },
        true, false, false, false };
    fixture.witness->applyToBuffer(context);
    CHECK(fixture.witness->serial.load() == serial + 1);
    CHECK(fixture.witness->editEnd.load() == 48000 + blockSize);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        CHECK(std::memcmp(audio.getReadPointer(channel), originalAudio.getReadPointer(channel),
                          static_cast<std::size_t>(audio.getNumSamples()) * sizeof(float)) == 0);
    REQUIRE(midi.size() == 1);
    CHECK(midi.isAllNotesOff);
    CHECK(midi[0].getTimeStamp() == 0.0012345);
    REQUIRE(midi[0].getRawDataSize() == static_cast<int>(bytes.size()));
    CHECK(std::equal(midi[0].getRawData(), midi[0].getRawData() + bytes.size(), bytes.begin()));
    context.isPlaying = false;
    fixture.witness->applyToBuffer(context);
    CHECK(fixture.witness->serial.load() == serial + 1);
    fixture.witness->baseClassDeinitialise();
}

TEST_CASE("Valid and invalid clock publication preserve audio and mixed MIDI buffers")
{
    Fixture fixture(false);
    REQUIRE(fixture.clock->beginGeneration());
    fixture.transport().play(false);
    fixture.hostStep();
    juce::AudioBuffer<float> audio(3, blockSize);
    for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        for (int index = 0; index < audio.getNumSamples(); ++index)
            audio.setSample(channel, index, static_cast<float>(channel * 200 - index) / 1024.0f);
    const juce::AudioBuffer<float> originalAudio(audio);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(16, 43, static_cast<juce::uint8>(97)), 17);
    midi.addEvent(juce::MidiMessage::programChange(3, 7), 91);
    midi.addEvent(juce::MidiMessage::pitchWheel(7, 11111), 91);
    const juce::MidiBuffer originalMidi(midi);
    for (const bool valid : { true, false })
    {
        fixture.clock->processBlock(audio, midi);
        CHECK(fixture.snapshot().valid == valid);
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            CHECK(std::memcmp(audio.getReadPointer(channel), originalAudio.getReadPointer(channel),
                              static_cast<std::size_t>(audio.getNumSamples()) * sizeof(float)) == 0);
        REQUIRE(midi.data.size() == originalMidi.data.size());
        CHECK(std::equal(midi.data.begin(), midi.data.end(), originalMidi.data.begin()));
    }
}

TEST_CASE("Recording clock publication uses no heap operations on valid and fault paths")
{
    using Probe = composer::tests::AllocationProbe;
    if (!Probe::available())
        SKIP("Allocation observation requires the Windows debug runtime");
    Probe::arm();
    void* (* volatile allocate)(std::size_t) = &std::malloc;
    void* allocation = allocate(17);
    std::free(allocation);
    const auto positive = Probe::disarm();
    REQUIRE(positive >= 2);

    Fixture fixture(false);
    REQUIRE(fixture.clock->beginGeneration());
    fixture.transport().play(false);
    for (int repetition = 0; repetition < 8; ++repetition)
    {
        fixture.hostStep();
        Probe::arm();
        fixture.clock->processBlock(fixture.audio, fixture.midi);
        const auto operations = Probe::disarm();
        CHECK(operations == 0);
        CHECK(fixture.snapshot().valid);
    }
    Probe::arm();
    fixture.clock->processBlock(fixture.audio, fixture.midi);
    const auto faultOperations = Probe::disarm();
    CHECK(faultOperations == 0);
    CHECK_FALSE(fixture.snapshot().valid);
    fixture.clock->releaseResources();
    Probe::arm();
    fixture.clock->processBlock(fixture.audio, fixture.midi);
    const auto releasedOperations = Probe::disarm();
    CHECK(releasedOperations == 0);
    CHECK_FALSE(fixture.snapshot().valid);
}
