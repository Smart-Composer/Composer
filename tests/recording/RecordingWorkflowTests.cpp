#include "RecordingDevice.h"
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
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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
struct Driver
{
    using Callback = void (CALLBACK*)(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
    Callback callback = nullptr;
    DWORD_PTR instance = 0;
    double openedAt = 0;
    unsigned closedBeforeOpen = 0;
    std::atomic<bool> live{false};
    std::atomic<unsigned> opens{0}, prepares{0}, adds{0}, unprepares{0}, starts{0}, stops{0}, resets{0}, closes{0}, invalidCalls{0};
    std::array<LPMIDIHDR, 32> headers{};
};
std::array<Driver, 2> drivers;
std::atomic<unsigned> invalidHandles{0}, delivered{0}, refused{0}, unjoinedCloses{0};
std::jthread producer;
void join() { if (producer.joinable()) producer.join(); }
HMIDIIN handleFor(std::size_t index) { return reinterpret_cast<HMIDIIN>(static_cast<std::uintptr_t>(0x5a50 + index)); }
Driver* driverFor(HMIDIIN handle)
{
    for (std::size_t index = 0; index != drivers.size(); ++index)
        if (handle == handleFor(index) && drivers[index].live.load()) return &drivers[index];
    ++invalidHandles;
    return nullptr;
}
UINT WINAPI inputCount() { return 2; }
UINT WINAPI outputCount() { return 0; }
MMRESULT WINAPI inputCaps(UINT_PTR device, LPMIDIINCAPSW caps, UINT size)
{
    if (device >= drivers.size() || caps == nullptr || size < sizeof(MIDIINCAPSW)) return MMSYSERR_BADDEVICEID;
    *caps = {};
    wcsncpy_s(caps->szPname, device == 0 ? L"Composer selected test input" : L"Composer unrelated test input", _TRUNCATE);
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI inputMessage(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR) { return MMSYSERR_NOTSUPPORTED; }
MMRESULT WINAPI openInput(LPHMIDIIN handle, UINT device, DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
    if (device >= drivers.size() || !handle || !callback || !instance || flags != CALLBACK_FUNCTION) { ++invalidHandles; return MMSYSERR_ERROR; }
    auto& driver = drivers[device];
    if (driver.live.exchange(true)) { ++driver.invalidCalls; return MMSYSERR_ERROR; }
    driver.callback = reinterpret_cast<Driver::Callback>(callback);
    driver.instance = instance;
    driver.openedAt = juce::Time::getMillisecondCounterHiRes();
    driver.closedBeforeOpen = driver.closes.load();
    *handle = handleFor(device);
    driver.headers.fill(nullptr);
    ++driver.opens;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI prepareHeader(HMIDIIN handle, LPMIDIHDR header, UINT size)
{
    auto* driver = driverFor(handle);
    if (!driver || !header || size != sizeof(MIDIHDR)) return MMSYSERR_INVALPARAM;
    const auto index = driver->prepares.fetch_add(1) % 32;
    if (driver->headers[index] != nullptr) ++driver->invalidCalls;
    driver->headers[index] = header;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI addHeader(HMIDIIN handle, LPMIDIHDR header, UINT size)
{
    auto* driver = driverFor(handle);
    if (!driver || !header || size != sizeof(MIDIHDR)) return MMSYSERR_INVALPARAM;
    ++driver->adds;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI unprepareHeader(HMIDIIN handle, LPMIDIHDR header, UINT size)
{
    auto* driver = driverFor(handle);
    if (!driver || !header || size != sizeof(MIDIHDR)) return MMSYSERR_INVALPARAM;
    const auto index = driver->unprepares.fetch_add(1) % 32;
    if (driver->headers[index] != header) ++driver->invalidCalls;
    driver->headers[index] = nullptr;
    return MMSYSERR_NOERROR;
}
MMRESULT WINAPI startInput(HMIDIIN handle) { auto* d = driverFor(handle); if (!d) return MMSYSERR_INVALHANDLE; ++d->starts; return MMSYSERR_NOERROR; }
MMRESULT WINAPI stopInput(HMIDIIN handle) { auto* d = driverFor(handle); if (!d) return MMSYSERR_INVALHANDLE; if (d == &drivers[0]) join(); ++d->stops; return MMSYSERR_NOERROR; }
MMRESULT WINAPI resetInput(HMIDIIN handle) { auto* d = driverFor(handle); if (!d) return MMSYSERR_INVALHANDLE; ++d->resets; return MMSYSERR_NOERROR; }
MMRESULT WINAPI closeInput(HMIDIIN handle) { auto* d = driverFor(handle); if (!d) return MMSYSERR_INVALHANDLE; if (d == &drivers[0] && producer.joinable()) ++unjoinedCloses;
    d->live.store(false); d->callback = nullptr; d->instance = 0; ++d->closes; return MMSYSERR_NOERROR; }

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
    static_assert(sizeof(void*) == 8);
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


bool emit(DWORD_PTR packed)
{
    auto& driver = drivers[0];
    if (!driver.live.load()) { ++refused; return false; }
    const auto elapsed = juce::Time::getMillisecondCounterHiRes() - driver.openedAt;
    driver.callback(handleFor(0), MIM_DATA, driver.instance, packed, static_cast<DWORD_PTR>(elapsed));
    ++delivered;
    return true;
}
}

namespace app = composer::app;
namespace contracts = composer::contracts;
namespace project = composer::project;
constexpr double initialRate = 48000.0;
constexpr int blockSize = 128;

struct ScratchDirectory
{
    const juce::File parent = juce::File::getCurrentWorkingDirectory();
    const juce::File directory = parent.getChildFile("composer-recording-workflow-" + juce::Uuid().toString());
    ScratchDirectory()
    {
        REQUIRE(directory.isAChildOf(parent));
        REQUIRE_FALSE(directory.exists());
        REQUIRE(directory.createDirectory().wasOk());
    }
    ~ScratchDirectory() { directory.deleteRecursively(); }
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
        : PropertyStorage("ComposerRecordingWorkflowTests-" + root.getFileName()), root_(std::move(root)) {}
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

struct Bytes
{
    std::array<std::uint8_t, 3> value{};
    int size = 0;
    bool operator==(const Bytes&) const = default;
};

class RawMonitor final : public tracktion::Plugin
{
public:
    static constexpr const char* xmlTypeName = "composer_test_recording_workflow_monitor";
    static const char* getPluginName() { return "Recording Workflow Monitor"; }
    explicit RawMonitor(tracktion::PluginCreationInfo info) : Plugin(info) {}
    ~RawMonitor() override { notifyListenersOfDeletion(); }
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
            if (size < 1 || size > 3 || count == received.size()) { overflow = true; continue; }
            auto& result = received[count++];
            result.size = size;
            std::copy_n(message.getRawData(), size, result.value.begin());
        }
    }
    std::array<Bytes, 1024> received{};
    std::size_t count = 0;
    bool overflow = false;
};

template<class Predicate> void until(Predicate ready)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    while (!ready() && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    REQUIRE(ready());
}

struct Fixture
{
    ScratchDirectory scratch;
    std::unique_ptr<tracktion::Engine> engine = std::make_unique<tracktion::Engine>(
        std::make_unique<ScratchSettings>(scratch.directory),
        std::make_unique<tracktion::UIBehaviour>(), std::make_unique<HostedOnly>());
    std::unique_ptr<app::ApplicationProject> owner;
    juce::Array<juce::MidiDeviceInfo> inventory;
    std::shared_ptr<tracktion::PhysicalMidiInputDevice> selected, other;
    explicit Fixture(bool nativePorts = false)
    {
        REQUIRE(engine->getPropertyStorage().getPropertiesFile().getFile().isAChildOf(scratch.directory));
        engine->getPluginManager().createBuiltInType<GraphObserver>();
        engine->getPluginManager().createBuiltInType<RawMonitor>();
        engine->getPluginManager().createBuiltInType<app::CaptureClockWitness>();
        auto& devices = engine->getDeviceManager();
        auto& hosted = devices.getHostedAudioDeviceInterface();
        tracktion::HostedAudioDeviceInterface::Parameters parameters;
        parameters.sampleRate = initialRate;
        parameters.blockSize = blockSize;
        parameters.useMidiDevices = nativePorts;
        parameters.inputChannels = 0;
        parameters.outputChannels = 2;
        hosted.initialise(parameters);
        hosted.prepareToPlay(initialRate, blockSize);
        devices.dispatchPendingUpdates();
        owner = std::make_unique<app::ApplicationProject>(*engine, initialRate);
        if (nativePorts)
        {
            inventory = juce::MidiInput::getAvailableDevices();
            REQUIRE(inventory.size() == 2);
            const auto id = [&](int index) { return "midiin_" + juce::String::toHexString(inventory[index].identifier.hashCode()); };
            until([&] { return devices.findMidiInputDeviceForID(id(0)) && devices.findMidiInputDeviceForID(id(1)); });
            selected = std::dynamic_pointer_cast<tracktion::PhysicalMidiInputDevice>(devices.findMidiInputDeviceForID(id(0)));
            other = std::dynamic_pointer_cast<tracktion::PhysicalMidiInputDevice>(devices.findMidiInputDeviceForID(id(1)));
            REQUIRE(selected); REQUIRE(other);
            REQUIRE(selected->isEnabled()); REQUIRE(other->isEnabled());
            REQUIRE(synthetic::drivers[0].live.load()); REQUIRE(synthetic::drivers[1].live.load());
            other->saveProps();
        }
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

void start(Fixture& fixture, app::RecordingDevice& lease, app::RecordingController& controller)
{
    requireController(controller.start(lease.openStoppedInput(), lease.dedicatedInput()), controller);
    REQUIRE(controller.status().state == app::RecordingState::preparing);
    REQUIRE(fixture.owner->session().isRecording());
    REQUIRE(synthetic::emit(0x613c90)); // Stopped while actual asynchronous preparation runs.
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
    const auto before = synthetic::delivered.load();
    synthetic::producer = std::jthread([&] {
        for (const auto& bytes : messages)
        {
            DWORD_PTR packed = 0;
            for (std::size_t index = 0; index < bytes.size(); ++index)
                packed |= static_cast<DWORD_PTR>(bytes[index]) << (8 * index);
            synthetic::emit(packed);
        }
    });
    synthetic::join();
    REQUIRE(synthetic::delivered.load() - before == messages.size());
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

}

TEST_CASE("A selected native lease records a full synthetic minute through the controller")
{
    synthetic::presentInput();
    ScratchDirectory files;
    const auto path = files.directory.getChildFile("recorded-minute.composer");
    project::ProjectDocument expected;
    Rendered originalAudio;
    std::string originalToken;
    std::vector<std::vector<std::uint8_t>> wanted;
    {
        Fixture fixture(true);
        auto& owner = *fixture.owner;
        auto& devices = fixture.engine->getDeviceManager();
        devices.setDefaultMidiInDevice(fixture.selected->getDeviceID());
        until([&] { return devices.getDefaultMidiInDeviceID() == fixture.selected->getDeviceID(); });
        const auto settingsFor = [&](const auto& input) {
            auto xml = fixture.engine->getPropertyStorage().getXmlPropertyItem(tracktion::SettingID::midiin, input->getName());
            REQUIRE(xml);
            return xml->toString();
        };
        const auto unrelatedSettings = settingsFor(fixture.other);
        const auto userVirtuals = [&] {
            juce::StringArray names;
            names.addTokens(fixture.engine->getPropertyStorage().getProperty(tracktion::SettingID::virtualmididevices).toString(), ";", {});
            names.removeEmptyStrings();
            names.removeString("All MIDI Ins");
            names.removeString("all_midi_in");
            return names.joinIntoString(";");
        };
        const auto originalVirtuals = userVirtuals();
        auto patch = owner.session().document().patch;
        patch.waveform = contracts::Waveform::square;
        requireApplied(owner.apply(commandFor(owner, patch)));
        const auto revisionBefore = owner.session().context().revision;
        owner.playbackEdit().getTransport().ensureContextAllocated(true);
        REQUIRE(owner.playbackEdit().getTransport().getCurrentPlaybackContext() != nullptr);
        REQUIRE_FALSE(owner.playbackEdit().getAllInputDevices().isEmpty());
        app::RecordingDevice lease(owner);
        REQUIRE(lease.select(fixture.inventory[0].identifier));
        CHECK(owner.playbackEdit().getTransport().getCurrentPlaybackContext() == nullptr);
        CHECK_FALSE(fixture.selected->isEnabled());
        CHECK_FALSE(synthetic::drivers[0].live.load());
        CHECK(synthetic::drivers[0].opens.load() == 1);
        CHECK(synthetic::drivers[0].closes.load() == 1);
        CHECK(synthetic::drivers[1].opens.load() == 1);
        CHECK(synthetic::drivers[1].closes.load() == 0);
        until([&] { REQUIRE(lease.poll()); return lease.ready() && devices.getDefaultMidiInDeviceID() == "all_midi_in"; });
        const auto dedicated = lease.dedicatedInput();
        REQUIRE(dedicated);
        CHECK(dedicated->getMIDIInputSourceDevices().isEmpty());
        CHECK_FALSE(dedicated->useAllInputs);
        auto monitorPlugin = owner.playbackEdit().getPluginCache().createNewPlugin(RawMonitor::xmlTypeName, {});
        auto* monitor = dynamic_cast<RawMonitor*>(monitorPlugin.get());
        REQUIRE(monitor != nullptr);
        owner.playbackTrack().pluginList.insertPlugin(monitorPlugin, 0, nullptr);
        monitorPlugin = nullptr; // The edit owns the observer through capture completion.
        app::RecordingController controller(owner, 1024, 5000, 5000);
        start(fixture, lease, controller);
        CHECK(synthetic::drivers[0].opens.load() == 2);
        CHECK(synthetic::drivers[0].closes.load() == 1);
        CHECK(synthetic::drivers[0].closedBeforeOpen == 1);
        CHECK(synthetic::drivers[1].opens.load() == 1);
        CHECK(synthetic::drivers[1].closes.load() == 0);
        REQUIRE(lease.current());
        monitor->count = 0;
        monitor->overflow = false;
        double livePeak = 0;
        for (int second = 0; second < 60; ++second)
        {
            const auto channel = second % 16;
            const auto status = [channel](int kind) { return static_cast<std::uint8_t>(kind | channel); };
            const auto pitch = static_cast<std::uint8_t>(48 + channel);
            const std::vector<std::vector<std::uint8_t>> onset {
                {status(0x90), pitch, 97}, {status(0x90), pitch, 83}, {status(0x80), 111, 41},
                {status(0xa0), pitch, 73}, {status(0xb0), 1, 64}, {status(0xc0), 19},
                {status(0xd0), 55}, {status(0xe0), 12, 65}};
            emit(onset);
            wanted.insert(wanted.end(), onset.begin(), onset.end());
            livePeak = std::max(livePeak, advance(fixture, controller, 94));
            const std::vector<std::vector<std::uint8_t>> release {{status(0x80), pitch, 37}};
            emit(release);
            wanted.insert(wanted.end(), release.begin(), release.end());
            advance(fixture, controller, 281); // 375 blocks = one second at 48 kHz.
            REQUIRE(lease.poll());
            REQUIRE(lease.current());
            REQUIRE(controller.status().state == app::RecordingState::recording);
            REQUIRE(settingsFor(fixture.other) == unrelatedSettings);
            REQUIRE(synthetic::drivers[1].closes.load() == 0);
        }
        CHECK(livePeak > 0.001);
        REQUIRE(wanted.size() == 540);
        REQUIRE_FALSE(monitor->overflow);
        REQUIRE(monitor->count == wanted.size());
        for (std::size_t index = 0; index < wanted.size(); ++index)
        {
            REQUIRE(monitor->received[index].size == static_cast<int>(wanted[index].size()));
            CHECK(std::equal(wanted[index].begin(), wanted[index].end(), monitor->received[index].value.begin()));
        }
        requireController(controller.stop(), controller);
        CHECK(controller.status().state == app::RecordingState::idle);
        CHECK(controller.status().completed == contracts::EditOutcome::applied);
        CHECK(controller.status().preOriginEvents == 0);
        CHECK(owner.session().context().revision == revisionBefore + 1);
        CHECK_FALSE(owner.session().isRecording());
        CHECK(devices.getGlobalOutputAudioProcessor() == nullptr);
        CHECK_FALSE(synthetic::drivers[0].live.load());
        CHECK(synthetic::drivers[0].closes.load() == 2);
        CHECK_FALSE(synthetic::emit(0x613c90));
        CHECK(synthetic::refused.load() == 1);
        CHECK(lease.current());
        expected = owner.session().document();
        REQUIRE(expected.events.size() == wanted.size());
        for (std::size_t index = 0; index < wanted.size(); ++index)
        {
            CHECK(expected.events[index].bytes == wanted[index]);
            CHECK(std::isfinite(expected.events[index].timeSeconds));
            CHECK(expected.events[index].timeSeconds >= 0.0);
            CHECK(expected.events[index].timeSeconds <= expected.durationSeconds);
            if (index > 0) CHECK(expected.events[index].timeSeconds >= expected.events[index - 1].timeSeconds);
        }
        CHECK(expected.durationSeconds >= 60.0);
        CHECK(expected.durationSeconds < 60.1);
        CHECK(expected.durationSeconds > expected.events.back().timeSeconds + 0.5);
        originalToken = owner.session().context().projectInstanceId;
        REQUIRE(std::holds_alternative<project::SaveReceipt>(owner.save(path)));
        CHECK_FALSE(owner.session().isDirty());
        REQUIRE(std::get<project::ProjectDocument>(project::loadProjectFile(path)) == expected);
        const auto canonical = project::encodeProject(expected);
        REQUIRE(std::holds_alternative<std::string>(canonical));
        CHECK(path.loadFileAsString().toStdString() == std::get<std::string>(canonical));
        REQUIRE(lease.release());
        until([&] { return devices.getDefaultMidiInDeviceID() == fixture.selected->getDeviceID()
            && !devices.findMidiInputDeviceForID(dedicated->getDeviceID()); });
        CHECK(fixture.selected->isEnabled());
        CHECK(fixture.other->isEnabled());
        CHECK(synthetic::drivers[0].opens.load() == 3);
        CHECK(synthetic::drivers[0].closes.load() == 2);
        CHECK(synthetic::drivers[0].closedBeforeOpen == 2);
        CHECK(synthetic::drivers[1].opens.load() == 1);
        CHECK(synthetic::drivers[1].closes.load() == 0);
        CHECK(settingsFor(fixture.other) == unrelatedSettings);
        CHECK(userVirtuals() == originalVirtuals);
        CHECK_FALSE(lease.status().restorationSkipped);
        CHECK_FALSE(lease.status().defaultRestorationSkipped);
        CHECK(fixture.engine->getPropertyStorage().getXmlPropertyItem(tracktion::SettingID::virtualmidiin, dedicated->getName()) == nullptr);
        originalAudio = render(fixture, 60.5);
        CHECK(originalAudio.audio.size() == 2904000);
        CHECK(owner.session().document() == expected);
    }
    for (const auto& driver : synthetic::drivers)
    {
        CHECK_FALSE(driver.live.load());
        CHECK(driver.invalidCalls.load() == 0);
        CHECK(driver.opens.load() == driver.closes.load());
        CHECK(driver.prepares.load() == driver.opens.load() * 32);
        CHECK(driver.adds.load() == driver.prepares.load());
        CHECK(driver.unprepares.load() == driver.prepares.load());
        CHECK(driver.starts.load() == driver.opens.load());
        CHECK(driver.stops.load() == driver.closes.load());
        CHECK(driver.resets.load() == driver.closes.load());
    }
    CHECK(synthetic::invalidHandles.load() == 0);
    CHECK(synthetic::unjoinedCloses.load() == 0);
    CHECK_FALSE(synthetic::producer.joinable());
    CHECK(synthetic::delivered.load() == wanted.size() + 1); // One unadmitted preparation message.
    {
        Fixture fixture;
        auto& owner = *fixture.owner;
        requireOk(owner.open(path));
        CHECK(owner.session().context().projectInstanceId != originalToken);
        CHECK(owner.session().context().revision == 0);
        CHECK(owner.session().document() == expected);
        CHECK(identicalAudio(originalAudio, render(fixture, 60.5)));
    }
}
