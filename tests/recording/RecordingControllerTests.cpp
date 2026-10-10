#include "RecordingController.h"
#include "ApplicationProject.h"
#include "CaptureClock.h"
#include "InstrumentAdapter.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <cwchar>
#include <initializer_list>
#include <thread>

// This executable redirects its own WinMM exports before creating any engine or
// MIDI input. Keep these tests in a separate executable from other MIDI tests.
// JUCE still owns the native input thread, callback registration and teardown;
// the stand-in supplies messages without opening a hardware device.

namespace
{
namespace synthetic
{
const auto syntheticHandle = reinterpret_cast<HMIDIIN>(static_cast<std::uintptr_t>(0x5a5a));
constexpr const wchar_t* syntheticName = L"Composer synthetic input";

struct Driver
{
    using Callback = void (CALLBACK*)(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
    Callback callback = nullptr;
    DWORD_PTR instance = 0;
    double openedAt = 0;
    std::atomic<bool> live{false};
    std::atomic<unsigned> opens{0}, prepares{0}, adds{0}, unprepares{0}, starts{0}, stops{0}, resets{0}, closes{0};
    std::atomic<unsigned> invalidCalls{0}, unjoinedCleanup{0}, delivered{0}, refused{0};
    std::array<LPMIDIHDR, 32> headers{};
    std::jthread producer;

    bool valid(HMIDIIN handle)
    {
        if (handle == syntheticHandle && live.load()) return true;
        ++invalidCalls;
        return false;
    }
    bool emit(DWORD_PTR packed)
    {
        // The fixture joins its producer before closing the input.
        // After close it refuses without touching the saved native instance.
        if (!live.load()) { ++refused; return false; }
        const auto elapsedMs = (juce::Time::getMillisecondCounterHiRes() * 0.001 - openedAt) * 1000.0;
        callback(syntheticHandle, MIM_DATA, instance, packed, static_cast<DWORD_PTR>(elapsedMs));
        ++delivered;
        return true;
    }
    void join() { if (producer.joinable()) producer.join(); }
} driver;

UINT WINAPI inputCount() { return 1; }
UINT WINAPI outputCount() { return 0; }
MMRESULT WINAPI inputCaps(UINT_PTR device, LPMIDIINCAPSW caps, UINT size)
{
    if (device != 0 || caps == nullptr || size < sizeof(MIDIINCAPSW)) return MMSYSERR_BADDEVICEID;
    *caps = {};
    wcsncpy_s(caps->szPname, syntheticName, _TRUNCATE);
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI inputMessage(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR) { return MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI openInput(LPHMIDIIN handle, UINT device, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
    if (device != 0 || handle == nullptr || callback == 0 || instance == 0
        || flags != CALLBACK_FUNCTION || driver.live.load())
    {
        ++driver.invalidCalls;
        return MMSYSERR_ERROR;
    }
    driver.callback = reinterpret_cast<Driver::Callback>(callback);
    driver.openedAt = juce::Time::getMillisecondCounterHiRes() * 0.001;
    driver.instance = instance;
    *handle = syntheticHandle;
    driver.live.store(true);
    ++driver.opens;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI prepareHeader(HMIDIIN handle, LPMIDIHDR header, UINT size)
{
    if (!driver.valid(handle) || header == nullptr || size != sizeof(MIDIHDR)) return MMSYSERR_INVALPARAM;
    const auto index = driver.prepares.fetch_add(1);
    if (index >= driver.headers.size()) { ++driver.invalidCalls; return MMSYSERR_ERROR; }
    driver.headers[index] = header;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI addHeader(HMIDIIN handle, LPMIDIHDR header, UINT size)
{
    if (!driver.valid(handle) || header == nullptr || size != sizeof(MIDIHDR)) return MMSYSERR_INVALPARAM;
    ++driver.adds;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI unprepareHeader(HMIDIIN handle, LPMIDIHDR header, UINT size)
{
    if (!driver.valid(handle) || header == nullptr || size != sizeof(MIDIHDR)) return MMSYSERR_INVALPARAM;
    const auto index = driver.unprepares.fetch_add(1);
    if (index >= driver.headers.size() || driver.headers[index] != header) ++driver.invalidCalls;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI startInput(HMIDIIN handle)
{
    if (!driver.valid(handle)) return MMSYSERR_INVALHANDLE;
    ++driver.starts;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI stopInput(HMIDIIN handle)
{
    if (!driver.valid(handle)) return MMSYSERR_INVALHANDLE;
    driver.join();
    ++driver.stops;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI resetInput(HMIDIIN handle)
{
    if (!driver.valid(handle)) return MMSYSERR_INVALHANDLE;
    ++driver.resets;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI closeInput(HMIDIIN handle)
{
    if (!driver.valid(handle)) return MMSYSERR_INVALHANDLE;
    if (driver.producer.joinable()) ++driver.unjoinedCleanup;
    driver.live.store(false);
    driver.callback = nullptr;
    driver.instance = 0;
    ++driver.closes;
    return MMSYSERR_NOERROR;
}

void redirect(const char* name, const void* standIn)
{
    const auto module = GetModuleHandleW(L"winmm.dll");
    REQUIRE(module != nullptr);
    auto* target = reinterpret_cast<std::uint8_t*>(GetProcAddress(module, name));
    REQUIRE(target != nullptr);
    std::array<std::uint8_t, 12> jump {0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0};
    std::memcpy(jump.data() + 2, &standIn, sizeof(standIn));
    DWORD previous = 0;
    REQUIRE(VirtualProtect(target, jump.size(), PAGE_EXECUTE_READWRITE, &previous));
    std::memcpy(target, jump.data(), jump.size());
    REQUIRE(VirtualProtect(target, jump.size(), previous, &previous));
    REQUIRE(FlushInstructionCache(GetCurrentProcess(), target, jump.size()));
}
void presentInput()
{
    static_assert(sizeof(void*) == 8, "WinMM test redirection requires x64");
    REQUIRE_FALSE(driver.live.load());
    REQUIRE_FALSE(driver.producer.joinable());
    for (auto* counter : { &driver.opens, &driver.prepares, &driver.adds,
                           &driver.unprepares, &driver.starts, &driver.stops,
                           &driver.resets, &driver.closes, &driver.invalidCalls,
                           &driver.unjoinedCleanup, &driver.delivered, &driver.refused })
        counter->store(0);
    driver.headers.fill(nullptr);
    redirect("midiInGetNumDevs", reinterpret_cast<const void*>(&inputCount));
    redirect("midiOutGetNumDevs", reinterpret_cast<const void*>(&outputCount));
    redirect("midiInGetDevCapsW", reinterpret_cast<const void*>(&inputCaps));
    redirect("midiInMessage", reinterpret_cast<const void*>(&inputMessage));
    redirect("midiInOpen", reinterpret_cast<const void*>(&openInput));
    redirect("midiInPrepareHeader", reinterpret_cast<const void*>(&prepareHeader));
    redirect("midiInAddBuffer", reinterpret_cast<const void*>(&addHeader));
    redirect("midiInUnprepareHeader", reinterpret_cast<const void*>(&unprepareHeader));
    redirect("midiInStart", reinterpret_cast<const void*>(&startInput));
    redirect("midiInStop", reinterpret_cast<const void*>(&stopInput));
    redirect("midiInReset", reinterpret_cast<const void*>(&resetInput));
    redirect("midiInClose", reinterpret_cast<const void*>(&closeInput));
}

std::unique_ptr<juce::MidiInput> openStopped()
{
    const auto devices = juce::MidiInput::getAvailableDevices();
    REQUIRE(devices.size() == 1);
    REQUIRE(devices[0].name == juce::String(syntheticName));
    auto input = juce::MidiInput::openDevice(devices[0].identifier, nullptr);
    REQUIRE(input != nullptr);
    REQUIRE(driver.starts.load() == 1);
    REQUIRE(driver.prepares.load() == 32);
    REQUIRE(driver.adds.load() == 32);
    return input;
}

void checkCleanup()
{
    CHECK(driver.opens.load() == 1);
    CHECK(driver.starts.load() == 1);
    CHECK(driver.stops.load() == 1);
    CHECK(driver.resets.load() == 1);
    CHECK(driver.closes.load() == 1);
    CHECK(driver.prepares.load() == 32);
    CHECK(driver.adds.load() == 32);
    CHECK(driver.unprepares.load() == 32);
    CHECK(driver.invalidCalls.load() == 0);
    CHECK(driver.unjoinedCleanup.load() == 0);
    CHECK_FALSE(driver.live.load());
    CHECK_FALSE(driver.producer.joinable());
    CHECK_FALSE(driver.emit(0x613c90));
    CHECK(driver.refused.load() == 1);
}

}

namespace app = composer::app;
namespace contracts = composer::contracts;
namespace project = composer::project;
constexpr double initialRate = 48000.0;
constexpr int blockSize = 128;

struct ScratchDirectory
{
    const juce::File parent = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const juce::File directory = parent.getChildFile("composer-controller-" + juce::Uuid().toString());
    ScratchDirectory()
    {
        REQUIRE(directory.isAChildOf(parent));
        REQUIRE_FALSE(directory.exists());
        REQUIRE(directory.createDirectory().wasOk());
    }
    ~ScratchDirectory()
    {
        if (directory.isAChildOf(parent) && directory.getFileName().startsWith("composer-controller-"))
            directory.deleteRecursively();
    }
};

// Observes the running engine immediately after the instrument. It does not
// schedule messages or call the instrument; all storage precedes graph startup.
class GraphObserver final : public tracktion::Plugin
{
public:
    static constexpr const char* xmlTypeName = "composer_test_project_observer";
    static const char* getPluginName() { return "Project Playback Observer"; }
    explicit GraphObserver(tracktion::PluginCreationInfo info) : Plugin(info) {}
    ~GraphObserver() override { notifyListenersOfDeletion(); }
    juce::String getName() const override { return getPluginName(); }
    juce::String getPluginType() override { return xmlTypeName; }
    juce::String getSelectableDescription() override { return getName(); }
    BusLayout getBusses() const override { return BusLayout::singleStereoInOut(); }
    bool takesMidiInput() override { return true; }
    bool takesAudioInput() override { return true; }
    int getNumOutputChannelsGivenInputs(int) override { return 2; }
    void initialise(const tracktion::PluginInitialisationInfo& info) override { rate = info.sampleRate; }
    void deinitialise() override { ++deinitialisations; }
    void applyToBuffer(const tracktion::PluginRenderContext& context) override
    {
        if (!context.isPlaying) return;
        ++contexts;
        if (rate != expectedRate || context.destBuffer == nullptr
            || context.destBuffer->getNumChannels() < 2 || context.bufferStartSample < 0
            || context.bufferNumSamples < 0 || context.bufferStartSample > context.destBuffer->getNumSamples()
            || context.bufferNumSamples > context.destBuffer->getNumSamples() - context.bufferStartSample)
        {
            ++invalidContexts;
            return;
        }
        const auto first = std::llround(context.editTime.getStart().inSeconds() * rate);
        for (int offset = 0; offset < context.bufferNumSamples; ++offset)
        {
            const auto sample = first + offset;
            if (sample < 0 || static_cast<std::size_t>(sample) >= audio.size()) continue;
            const auto index = static_cast<std::size_t>(sample);
            if (coverage[index] != 0) ++duplicates;
            coverage[index] = 1;
            for (int channel = 0; channel < 2; ++channel)
                audio[index][static_cast<std::size_t>(channel)] = context.destBuffer->getSample(channel, context.bufferStartSample + offset);
        }
    }

    std::vector<std::array<float, 2>> audio;
    std::vector<std::uint8_t> coverage;
    double expectedRate = 0.0;
    double rate = 0.0;
    std::size_t contexts = 0, invalidContexts = 0, duplicates = 0;
    int deinitialisations = 0;
};

class ScratchSettings final : public tracktion::PropertyStorage
{
public:
    explicit ScratchSettings(juce::File root)
        : PropertyStorage("ComposerControllerTests-" + root.getFileName()), root_(std::move(root)) {}
    juce::File getAppCacheFolder() override { return root_.getChildFile("cache"); }
    juce::File getAppPrefsFolder() override { return root_.getChildFile("preferences"); }
private:
    juce::File root_;
};

class HostedOnly final : public tracktion::EngineBehaviour
{
public:
    bool autoInitialiseDeviceManager() override { return false; }
    bool addSystemAudioIODeviceTypes() override { return false; }
};

struct Fixture
{
    ScratchDirectory scratch;
    std::unique_ptr<tracktion::Engine> engine = std::make_unique<tracktion::Engine>(
        std::make_unique<ScratchSettings>(scratch.directory),
        std::make_unique<tracktion::UIBehaviour>(), std::make_unique<HostedOnly>());
    std::unique_ptr<app::ApplicationProject> owner;
    Fixture()
    {
        REQUIRE(engine->getPropertyStorage().getPropertiesFile().getFile().isAChildOf(scratch.directory));
        engine->getPluginManager().createBuiltInType<GraphObserver>();
        engine->getPluginManager().createBuiltInType<app::CaptureClockWitness>();
        auto& devices = engine->getDeviceManager();
        auto& hosted = devices.getHostedAudioDeviceInterface();
        tracktion::HostedAudioDeviceInterface::Parameters parameters;
        parameters.sampleRate = initialRate;
        parameters.blockSize = blockSize;
        parameters.useMidiDevices = false;
        parameters.inputChannels = 0;
        parameters.outputChannels = 2;
        hosted.initialise(parameters);
        hosted.prepareToPlay(initialRate, blockSize);
        devices.dispatchPendingUpdates();
        owner = std::make_unique<app::ApplicationProject>(*engine, initialRate);
    }
};

contracts::ProjectCommand commandFor(const app::ApplicationProject& owner, const contracts::InstrumentPatch& patch)
{
    const auto context = owner.session().context();
    return {context.projectInstanceId, context.revision, patch};
}

template <class T>
void requireApplied(const app::ApplicationResult<T>& result)
{
    REQUIRE(std::holds_alternative<contracts::EditOutcome>(result));
    REQUIRE(std::get<contracts::EditOutcome>(result) == contracts::EditOutcome::applied);
}

void requireOk(const app::ApplicationStatus& result)
{
    REQUIRE(std::holds_alternative<std::monostate>(result));
}

struct Rendered
{
    std::vector<std::array<float, 2>> audio;
    double peak = 0.0;
};

Rendered render(Fixture& fixture, double seconds)
{
    auto& owner = *fixture.owner;
    const auto patch = owner.session().document().patch;
    auto observerPlugin = owner.playbackEdit().getPluginCache().createNewPlugin(GraphObserver::xmlTypeName, {});
    auto* observer = dynamic_cast<GraphObserver*>(observerPlugin.get());
    REQUIRE(observer != nullptr);
    observer->expectedRate = owner.playbackSampleRate();
    const auto sampleCount = static_cast<std::size_t>(std::llround(seconds * observer->expectedRate));
    observer->audio.resize(sampleCount);
    observer->coverage.resize(sampleCount, 0);
    owner.playbackTrack().pluginList.insertPlugin(observerPlugin, 1, nullptr);
    auto* adapter = owner.playbackTrack().pluginList.findFirstPluginOfType<app::InstrumentAdapter>();
    REQUIRE(adapter != nullptr);
    auto& devices = fixture.engine->getDeviceManager();
    auto& hosted = devices.getHostedAudioDeviceInterface();
    auto& transport = owner.playbackEdit().getTransport();
    transport.ensureContextAllocated(true);
    devices.dispatchPendingUpdates();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    requireOk(owner.playFromStart());
    REQUIRE(transport.isPlaying());
    juce::AudioBuffer<float> output(2, blockSize);
    juce::MidiBuffer midi;
    const auto callbackLimit = sampleCount / blockSize + 2048;
    bool finished = false, finite = true, contextPresent = true;
    double hostedPeak = 0.0;
    for (std::size_t index = 0; index < callbackLimit; ++index)
    {
        const auto* context = transport.getCurrentPlaybackContext();
        if (context == nullptr) { contextPresent = false; break; }
        if (context->globalStreamTimeToEditTime(devices.getCurrentStreamTime()).inSeconds() >= seconds + 0.02)
        {
            finished = true;
            break;
        }
        output.clear();
        midi.clear();
        hosted.processBlock(output, midi);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < blockSize; ++sample)
            {
                const auto value = output.getSample(channel, sample);
                finite &= std::isfinite(value);
                hostedPeak = std::max(hostedPeak, static_cast<double>(std::abs(value)));
            }
        if (index % 256 == 0) juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
    }
    REQUIRE(finished);
    REQUIRE(contextPresent);
    REQUIRE(finite);
    REQUIRE(hostedPeak > 0.001);
    REQUIRE(observer->contexts > 0);
    REQUIRE(observer->invalidContexts == 0);
    REQUIRE(observer->duplicates == 0);
    REQUIRE(static_cast<std::size_t>(std::count(observer->coverage.begin(), observer->coverage.end(), std::uint8_t{1})) == sampleCount);
    REQUIRE(adapter->consumeRenderFaults() == 0);
    owner.stop();
    REQUIRE_FALSE(transport.isPlaying());
    REQUIRE(transport.getCurrentPlaybackContext() == nullptr);
    REQUIRE(observer->deinitialisations > 0);
    REQUIRE(owner.playbackPatch() == patch);
    Rendered result{std::move(observer->audio)};
    for (const auto& sample : result.audio)
        for (const float value : sample)
        {
            finite &= std::isfinite(value);
            result.peak = std::max(result.peak, static_cast<double>(std::abs(value)));
        }
    REQUIRE(finite);
    REQUIRE(result.peak > 0.001);
    observer->removeFromParent();
    return result;
}

bool identicalAudio(const Rendered& left, const Rendered& right)
{
    return left.audio.size() == right.audio.size()
        && std::memcmp(left.audio.data(), right.audio.data(), left.audio.size() * sizeof(left.audio[0])) == 0;
}

struct DedicatedInput
{
    Fixture& fixture;
    std::shared_ptr<tracktion::VirtualMidiInputDevice> input;
    explicit DedicatedInput(Fixture& value) : fixture(value)
    {
        auto& devices = fixture.engine->getDeviceManager();
        const auto name = "Native Take " + juce::Uuid().toString();
        const auto id = "vmidiin_" + juce::String::toHexString(name.hashCode());
        REQUIRE(devices.createVirtualMidiDevice(name).wasOk());
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
        while (!input && juce::Time::getMillisecondCounterHiRes() < deadline)
        {
            devices.dispatchPendingUpdates();
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            input = std::dynamic_pointer_cast<tracktion::VirtualMidiInputDevice>(devices.findMidiInputDeviceForID(id));
        }
        REQUIRE(input != nullptr);
        REQUIRE_FALSE(input->useAllInputs);
        input->setMIDIInputSourceDevices({});
        input->setEnabled(true);
        input->setMonitorMode(tracktion::InputDevice::MonitorMode::on);
        for (const auto& device : devices.getMidiInDevices())
            REQUIRE(device->getDeviceType() != tracktion::InputDevice::physicalMidiDevice);
    }
};

double step(Fixture& fixture)
{
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    audio.clear();
    fixture.engine->getDeviceManager().getHostedAudioDeviceInterface().processBlock(audio, midi);
    double peak = 0;
    bool finite = true;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < blockSize; ++sample)
        {
            const auto value = audio.getSample(channel, sample);
            finite &= std::isfinite(value);
            peak = std::max(peak, static_cast<double>(std::abs(value)));
        }
    REQUIRE(finite);
    return peak;
}

void requireController(bool result, const app::RecordingController& controller)
{
    const auto state = controller.status();
    INFO("Controller state=" << static_cast<int>(state.state) << " failure=" << static_cast<int>(state.failure));
    if (const auto exception = controller.exception())
    {
        try { std::rethrow_exception(exception); }
        catch (const std::exception& error) { INFO("Controller exception: " << error.what()); REQUIRE(result); }
    }
    REQUIRE(result);
}

void start(Fixture& fixture, const DedicatedInput& route, app::RecordingController& controller)
{
    requireController(controller.start(synthetic::openStopped(), route.input), controller);
    REQUIRE(controller.status().state == app::RecordingState::preparing);
    REQUIRE(fixture.owner->session().isRecording());
    REQUIRE(synthetic::driver.emit(0x613c90)); // Stopped while actual asynchronous preparation runs.
    std::optional<app::CaptureClockSnapshot> lastPreparing;
    for (int iteration = 0; iteration < 512 && controller.status().state == app::RecordingState::preparing; ++iteration)
    {
        step(fixture);
        fixture.engine->getDeviceManager().dispatchPendingUpdates();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
        requireController(controller.poll(), controller);
        auto* clock = dynamic_cast<app::CaptureClock*>(fixture.engine->getDeviceManager().getGlobalOutputAudioProcessor());
        REQUIRE(clock != nullptr);
        if (controller.status().state == app::RecordingState::preparing)
        {
            requireController(controller.poll(), controller);
            requireController(controller.poll(), controller);
            REQUIRE(controller.status().state == app::RecordingState::preparing);
            lastPreparing = clock->read();
        }
        else
        {
            const auto ready = clock->read();
            REQUIRE(lastPreparing.has_value());
            REQUIRE(ready.has_value());
            REQUIRE(lastPreparing->valid);
            REQUIRE(ready->valid);
            CHECK(ready->generation == lastPreparing->generation);
            CHECK(ready->rate == lastPreparing->rate);
            CHECK(ready->serial > lastPreparing->serial);
            CHECK(ready->streamEnd > lastPreparing->streamEnd);
            CHECK(ready->graphSerial > lastPreparing->graphSerial);
            CHECK(ready->graphEditEnd > lastPreparing->graphEditEnd);
        }
    }
    REQUIRE(controller.status().state == app::RecordingState::recording);
    REQUIRE(controller.pending() == nullptr);
    REQUIRE(fixture.engine->getDeviceManager().getGlobalOutputAudioProcessor() != nullptr);
    // Begin the musical test on a later audio tick. The stand-in reports real
    // elapsed whole milliseconds, so this also leaves room for its quantisation.
    step(fixture);
    requireController(controller.poll(), controller);
}

void emit(const std::vector<std::vector<std::uint8_t>>& messages)
{
    const auto before = synthetic::driver.delivered.load();
    synthetic::driver.producer = std::jthread([&] {
        for (const auto& bytes : messages)
        {
            DWORD_PTR packed = 0;
            for (std::size_t index = 0; index < bytes.size(); ++index)
                packed |= static_cast<DWORD_PTR>(bytes[index]) << (8 * index);
            synthetic::driver.emit(packed);
        }
    });
    synthetic::driver.join();
    REQUIRE(synthetic::driver.delivered.load() - before == messages.size());
}

double advance(Fixture& fixture, app::RecordingController& controller, int blocks)
{
    double peak = 0;
    for (int index = 0; index < blocks; ++index)
    {
        peak = std::max(peak, step(fixture));
        requireController(controller.poll(), controller);
    }
    return peak;
}

void verifyInterrupted(Fixture& fixture, app::RecordingController& controller, const project::ProjectDocument& original)
{
    CHECK_FALSE(controller.poll());
    CHECK(controller.status().state == app::RecordingState::pendingCompletion);
    CHECK(controller.status().failure == app::RecordingFailure::captureInterrupted);
    const auto* take = controller.pending();
    REQUIRE(take != nullptr);
    CHECK_FALSE(take->isComplete());
    REQUIRE(take->captured.events.size() == 1);
    CHECK(take->captured.events[0].bytes == std::vector<std::uint8_t>{0x90, 60, 97});
    CHECK(fixture.owner->session().document() == original);
    CHECK(fixture.owner->session().isRecording());
    CHECK(fixture.engine->getDeviceManager().getGlobalOutputAudioProcessor() == nullptr);
    CHECK(fixture.owner->playbackEdit().getTransport().getCurrentPlaybackContext() == nullptr);
    CHECK_FALSE(controller.retryCompletion());
    CHECK(controller.pending() == take);
    CHECK(fixture.owner->session().document() == original);
    requireController(controller.discardPending(), controller);
    CHECK(controller.status().state == app::RecordingState::idle);
    CHECK(controller.pending() == nullptr);
    CHECK_FALSE(fixture.owner->session().isRecording());
    CHECK(fixture.owner->session().document() == original);
    synthetic::checkCleanup();
}
}

TEST_CASE("The recording controller captures through native MIDI and saves a reopenable performance")
{
    synthetic::presentInput();
    ScratchDirectory files;
    const auto path = files.directory.getChildFile("native-controller.composer");
    project::ProjectDocument expected;
    Rendered originalAudio;
    std::string originalToken;
    std::vector<std::vector<std::uint8_t>> wanted;
    {
        Fixture fixture;
        auto& owner = *fixture.owner;
        auto patch = owner.session().document().patch;
        patch.waveform = contracts::Waveform::square;
        requireApplied(owner.apply(commandFor(owner, patch)));
        const auto initialRevision = owner.session().context().revision;
        DedicatedInput route(fixture);
        app::RecordingController controller(owner, 512, 5000, 5000);
        start(fixture, route, controller);
        for (int channel = 0; channel < 16; ++channel)
        {
            const auto status = [channel](int kind) { return static_cast<std::uint8_t>(kind | channel); };
            const auto pitch = static_cast<std::uint8_t>(48 + channel);
            wanted.push_back({status(0x90), pitch, 97});
            wanted.push_back({status(0x90), pitch, 83});
            wanted.push_back({status(0x80), 111, 41});
            wanted.push_back({status(0xa0), pitch, 73});
            wanted.push_back({status(0xb0), 1, 64});
            wanted.push_back({status(0xc0), 19});
            wanted.push_back({status(0xd0), 55});
            wanted.push_back({status(0xe0), 12, 65});
        }
        emit(wanted);
        const auto livePeak = advance(fixture, controller, 32);
        std::vector<std::vector<std::uint8_t>> releases;
        for (int channel = 0; channel < 16; ++channel)
            releases.push_back({static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(48 + channel), 37});
        emit(releases);
        wanted.insert(wanted.end(), releases.begin(), releases.end());
        advance(fixture, controller, 128);
        CHECK(livePeak > 0.001);
        requireController(controller.stop(), controller);
        CHECK(controller.status().state == app::RecordingState::idle);
        CHECK(controller.status().completed == contracts::EditOutcome::applied);
        CHECK(controller.pending() == nullptr);
        CHECK(owner.session().context().revision == initialRevision + 1);
        CHECK_FALSE(owner.session().isRecording());
        CHECK(owner.session().isDirty());
        CHECK(fixture.engine->getDeviceManager().getGlobalOutputAudioProcessor() == nullptr);
        expected = owner.session().document();
        REQUIRE(expected.events.size() == wanted.size());
        for (std::size_t index = 0; index < wanted.size(); ++index)
        {
            CHECK(expected.events[index].bytes == wanted[index]);
            CHECK(std::isfinite(expected.events[index].timeSeconds));
            CHECK(expected.events[index].timeSeconds >= 0.0);
            CHECK(expected.events[index].timeSeconds <= expected.durationSeconds);
        }
        CHECK(expected.durationSeconds > 0.3);
        CHECK(expected.durationSeconds < 0.6);
        CHECK(expected.durationSeconds > expected.events.back().timeSeconds);
        originalToken = owner.session().context().projectInstanceId;
        originalAudio = render(fixture, 0.8);
        REQUIRE(std::holds_alternative<project::SaveReceipt>(owner.save(path)));
        CHECK_FALSE(owner.session().isDirty());
        REQUIRE(std::get<project::ProjectDocument>(project::loadProjectFile(path)) == expected);
        const auto canonical = project::encodeProject(expected);
        REQUIRE(std::holds_alternative<std::string>(canonical));
        CHECK(path.loadFileAsString().toStdString() == std::get<std::string>(canonical));
        synthetic::checkCleanup();
    }
    {
        Fixture fixture;
        auto& owner = *fixture.owner;
        requireOk(owner.open(path));
        CHECK(owner.session().context().projectInstanceId != originalToken);
        CHECK(owner.session().context().revision == 0);
        CHECK(owner.session().undoHistory().empty());
        CHECK(owner.session().redoHistory().empty());
        CHECK_FALSE(owner.session().isDirty());
        CHECK(owner.session().document() == expected);
        CHECK(identicalAudio(originalAudio, render(fixture, 0.8)));
    }
}

TEST_CASE("The recording controller retains an incomplete take after a sample rate change")
{
    synthetic::presentInput();
    Fixture fixture;
    const auto original = fixture.owner->session().document();
    DedicatedInput route(fixture);
    app::RecordingController controller(*fixture.owner, 16, 5000, 5000);
    start(fixture, route, controller);
    emit({{0x90, 60, 97}});
    advance(fixture, controller, 2);
    fixture.engine->getDeviceManager().getHostedAudioDeviceInterface().prepareToPlay(44100, blockSize);
    verifyInterrupted(fixture, controller, original);
}

TEST_CASE("The recording controller retains an incomplete take after transport stops externally")
{
    synthetic::presentInput();
    Fixture fixture;
    const auto original = fixture.owner->session().document();
    DedicatedInput route(fixture);
    app::RecordingController controller(*fixture.owner, 16, 5000, 5000);
    start(fixture, route, controller);
    emit({{0x90, 60, 97}});
    advance(fixture, controller, 2);
    fixture.owner->playbackEdit().getTransport().stop(false, false);
    verifyInterrupted(fixture, controller, original);
}

TEST_CASE("The recording controller retains an incomplete take when its input is disabled")
{
    synthetic::presentInput();
    Fixture fixture;
    const auto original = fixture.owner->session().document();
    DedicatedInput route(fixture);
    app::RecordingController controller(*fixture.owner, 16, 5000, 5000);
    start(fixture, route, controller);
    emit({{0x90, 60, 97}});
    advance(fixture, controller, 2);
    route.input->setEnabled(false);
    verifyInterrupted(fixture, controller, original);
}

TEST_CASE("The recording controller cancels preparation before admitting native MIDI")
{
    synthetic::presentInput();
    Fixture fixture;
    const auto original = fixture.owner->session().document();
    DedicatedInput route(fixture);
    app::RecordingController controller(*fixture.owner, 16, 5000, 5000);
    requireController(controller.start(synthetic::openStopped(), route.input), controller);
    CHECK(controller.status().state == app::RecordingState::preparing);
    CHECK(fixture.owner->session().isRecording());
    REQUIRE(synthetic::driver.emit(0x613c90));
    requireController(controller.stop(), controller);
    CHECK(controller.status().state == app::RecordingState::idle);
    CHECK_FALSE(controller.status().completed.has_value());
    CHECK_FALSE(fixture.owner->session().isRecording());
    CHECK(fixture.owner->session().document() == original);
    CHECK(controller.pending() == nullptr);
    CHECK(fixture.engine->getDeviceManager().getGlobalOutputAudioProcessor() == nullptr);
    CHECK(fixture.owner->playbackEdit().getTransport().getCurrentPlaybackContext() == nullptr);
    synthetic::checkCleanup();
}

TEST_CASE("The recording controller retains an incomplete take after an external interruption")
{
    synthetic::presentInput();
    Fixture fixture;
    const auto original = fixture.owner->session().document();
    DedicatedInput route(fixture);
    app::RecordingController controller(*fixture.owner, 16, 5000, 5000);
    start(fixture, route, controller);
    emit({{0x90, 60, 97}});
    advance(fixture, controller, 2);
    CHECK_FALSE(controller.interrupt());
    REQUIRE(controller.pending() != nullptr);
    const auto* retained = controller.pending();
    CHECK_FALSE(controller.interrupt());
    CHECK(controller.pending() == retained);
    verifyInterrupted(fixture, controller, original);
}

TEST_CASE("The recording controller cancels preparation on external interruption")
{
    synthetic::presentInput();
    Fixture fixture;
    const auto original = fixture.owner->session().document();
    DedicatedInput route(fixture);
    app::RecordingController controller(*fixture.owner, 16, 5000, 5000);
    requireController(controller.start(synthetic::openStopped(), route.input), controller);
    REQUIRE(controller.status().state == app::RecordingState::preparing);
    REQUIRE(fixture.owner->session().isRecording());
    requireController(controller.interrupt(), controller);
    CHECK(controller.status().state == app::RecordingState::idle);
    CHECK_FALSE(fixture.owner->session().isRecording());
    CHECK(fixture.owner->session().document() == original);
    CHECK(controller.pending() == nullptr);
    CHECK(fixture.engine->getDeviceManager().getGlobalOutputAudioProcessor() == nullptr);
    synthetic::checkCleanup();
}
