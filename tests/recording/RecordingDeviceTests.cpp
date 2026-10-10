#include "RecordingDevice.h"

#include <memory>
#include <utility>

// This executable redirects its own WinMM exports to two successful synthetic
// inputs before constructing the engine. CTest runs each case in a fresh process.
// Keep it separate from other MIDI tests; no physical driver or settings are used.

#include <juce_audio_devices/juce_audio_devices.h>
#include <catch2/catch_test_macros.hpp>
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace
{
namespace synthetic
{
struct Driver
{
    std::atomic<bool> live{false};
    std::atomic<unsigned> opens{0}, prepares{0}, adds{0}, unprepares{0}, starts{0}, stops{0}, resets{0}, closes{0}, invalidCalls{0};
    std::array<LPMIDIHDR, 32> headers{};
};
std::array<Driver, 2> drivers;
std::atomic<unsigned> invalidHandles{0};
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
MMRESULT WINAPI stopInput(HMIDIIN handle) { auto* d = driverFor(handle); if (!d) return MMSYSERR_INVALHANDLE; ++d->stops; return MMSYSERR_NOERROR; }
MMRESULT WINAPI resetInput(HMIDIIN handle) { auto* d = driverFor(handle); if (!d) return MMSYSERR_INVALHANDLE; ++d->resets; return MMSYSERR_NOERROR; }
MMRESULT WINAPI closeInput(HMIDIIN handle) { auto* d = driverFor(handle); if (!d) return MMSYSERR_INVALHANDLE; d->live.store(false); ++d->closes; return MMSYSERR_NOERROR; }

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


}
}

namespace
{
namespace app = composer::app;
class ScratchSettings final : public tracktion::PropertyStorage
{
public:
    explicit ScratchSettings(juce::File root) : PropertyStorage("RecordingDevice-" + root.getFileName()), root_(std::move(root)) {}
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
template<class Predicate> void until(Predicate ready)
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    while (!ready() && juce::Time::getMillisecondCounterHiRes() < deadline)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(2);
    REQUIRE(ready());
}
struct Fixture
{
    juce::File directory = juce::File::getCurrentWorkingDirectory().getChildFile("device-" + juce::Uuid().toString());
    std::unique_ptr<tracktion::Engine> engine;
    std::unique_ptr<app::ApplicationProject> owner;
    juce::Array<juce::MidiDeviceInfo> inventory;
    std::shared_ptr<tracktion::PhysicalMidiInputDevice> selected, other;
    Fixture()
    {
        REQUIRE(directory.isAChildOf(juce::File::getCurrentWorkingDirectory()));
        REQUIRE(directory.createDirectory().wasOk());
        engine = std::make_unique<tracktion::Engine>(std::make_unique<ScratchSettings>(directory),
            std::make_unique<tracktion::UIBehaviour>(), std::make_unique<HostedOnly>());
        REQUIRE(engine->getPropertyStorage().getPropertiesFile().getFile().isAChildOf(directory));
        auto& devices = engine->getDeviceManager();
        auto& hosted = devices.getHostedAudioDeviceInterface();
        tracktion::HostedAudioDeviceInterface::Parameters parameters;
        parameters.sampleRate = 48000; parameters.blockSize = 128;
        parameters.useMidiDevices = true; parameters.inputChannels = 0; parameters.outputChannels = 2;
        hosted.initialise(parameters);
        hosted.prepareToPlay(48000, 128);
        devices.dispatchPendingUpdates();
        owner = std::make_unique<app::ApplicationProject>(*engine, 48000);
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
    juce::String otherSettings()
    {
        auto value = engine->getPropertyStorage().getXmlPropertyItem(tracktion::SettingID::midiin, other->getName());
        REQUIRE(value);
        return value->toString();
    }
    juce::String virtualList()
    {
        juce::StringArray list;
        list.addTokens(engine->getPropertyStorage().getProperty(tracktion::SettingID::virtualmididevices).toString(), ";", {});
        list.removeEmptyStrings(); list.removeString("All MIDI Ins"); list.removeString("all_midi_in");
        list.sort(false);
        return list.joinIntoString(";");
    }
};
void requireNativeCleanup()
{
    for (const auto& driver : synthetic::drivers)
    {
        CHECK(driver.invalidCalls.load() == 0);
        CHECK_FALSE(driver.live.load());
        CHECK(driver.opens.load() == driver.closes.load());
        CHECK(driver.prepares.load() == driver.opens.load() * 32);
        CHECK(driver.unprepares.load() == driver.prepares.load());
        CHECK(driver.stops.load() == driver.closes.load());
    }
    CHECK(synthetic::invalidHandles.load() == 0);
}
}

TEST_CASE("Device lease closes only its selected port and restores its automatic default fallback")
{
    synthetic::presentInput();
    {
        Fixture f;
        auto& devices = f.engine->getDeviceManager();
        devices.setDefaultMidiInDevice(f.selected->getDeviceID());
        until([&] { return devices.getDefaultMidiInDeviceID() == f.selected->getDeviceID(); });
        const auto otherSettings = f.otherSettings();
        const auto originalList = f.virtualList();
        auto& transport = f.owner->playbackEdit().getTransport();
        transport.ensureContextAllocated(true);
        REQUIRE(transport.getCurrentPlaybackContext() != nullptr);
        REQUIRE_FALSE(f.owner->playbackEdit().getAllInputDevices().isEmpty());
        app::RecordingDevice lease(*f.owner);
        const auto closeBefore = synthetic::drivers[0].closes.load();
        REQUIRE(lease.select(f.inventory[0].identifier));
        CHECK(transport.getCurrentPlaybackContext() == nullptr);
        CHECK_FALSE(f.selected->isEnabled());
        CHECK(synthetic::drivers[0].closes.load() == closeBefore + 1);
        CHECK(synthetic::drivers[1].closes.load() == 0);
        until([&] { REQUIRE(lease.poll()); return lease.ready() && devices.getDefaultMidiInDeviceID() == "all_midi_in"; });
        REQUIRE(lease.current());
        const auto dedicated = lease.dedicatedInput();
        REQUIRE(dedicated); CHECK_FALSE(dedicated->useAllInputs);
        CHECK(dedicated->getMIDIInputSourceDevices().isEmpty());
        CHECK(dedicated->getMonitorMode() == tracktion::InputDevice::MonitorMode::on);
        auto native = lease.openStoppedInput(); REQUIRE(native); native.reset();
        CHECK(synthetic::drivers[0].closes.load() == closeBefore + 2);
        CHECK(f.otherSettings() == otherSettings);
        REQUIRE(lease.release());
        until([&] { return devices.getDefaultMidiInDeviceID() == f.selected->getDeviceID()
            && !devices.findMidiInputDeviceForID(dedicated->getDeviceID()); });
        CHECK(f.selected->isEnabled()); CHECK(f.other->isEnabled());
        CHECK(f.virtualList() == originalList);
        CHECK(f.otherSettings() == otherSettings);
        CHECK_FALSE(lease.status().restorationSkipped);
        CHECK_FALSE(lease.status().defaultRestorationSkipped);
        CHECK(f.engine->getPropertyStorage().getXmlPropertyItem(tracktion::SettingID::virtualmidiin, dedicated->getName()) == nullptr);
        CHECK(synthetic::drivers[1].opens.load() == 1);
    }
    requireNativeCleanup();
}

TEST_CASE("Device lease preserves an independent default and cancels an unpublished virtual input")
{
    synthetic::presentInput();
    {
        Fixture f;
        auto& devices = f.engine->getDeviceManager();
        devices.setDefaultMidiInDevice(f.selected->getDeviceID());
        until([&] { return devices.getDefaultMidiInDeviceID() == f.selected->getDeviceID(); });
        app::RecordingDevice lease(*f.owner);
        const auto originalList = f.virtualList();
        REQUIRE(lease.select(f.inventory[0].identifier));
        REQUIRE(lease.release()); // No event loop has published the new virtual input.
        CHECK(f.virtualList() == originalList);
        until([&] { return f.selected->isEnabled() && synthetic::drivers[0].live.load(); });
        REQUIRE(lease.select(f.inventory[0].identifier));
        until([&] { REQUIRE(lease.poll()); return lease.ready() && devices.getDefaultMidiInDeviceID() == "all_midi_in"; });
        devices.setDefaultMidiInDevice(f.other->getDeviceID());
        until([&] { return devices.getDefaultMidiInDeviceID() == f.other->getDeviceID(); });
        REQUIRE(lease.release());
        CHECK(devices.getDefaultMidiInDeviceID() == f.other->getDeviceID());
        CHECK(f.engine->getPropertyStorage().getProperty(tracktion::SettingID::defaultMidiInDevice).toString() == f.other->getDeviceID());
        CHECK(lease.status().defaultRestorationSkipped);
        CHECK(f.virtualList() == originalList);
    }
    requireNativeCleanup();
}

TEST_CASE("Device lease preserves prior disabled state and rejects a second edit and active recording")
{
    synthetic::presentInput();
    {
        Fixture f;
        f.selected->setEnabled(false);
        until([&] { return !synthetic::drivers[0].live.load(); });
        const auto otherSettings = f.otherSettings();
        app::RecordingDevice lease(*f.owner);
        {
            app::ApplicationProject second(*f.engine, 48000);
            REQUIRE_FALSE(lease.select(f.inventory[0].identifier));
            CHECK(lease.status().failure == app::RecordingDeviceFailure::otherEdit);
            CHECK_FALSE(f.selected->isEnabled());
        }
        REQUIRE(lease.select(f.inventory[0].identifier));
        until([&] { REQUIRE(lease.poll()); return lease.ready(); });
        auto ticketResult = f.owner->beginRecording();
        REQUIRE(std::holds_alternative<std::unique_ptr<composer::project::RecordingTicket>>(ticketResult));
        const auto& ticket = *std::get<std::unique_ptr<composer::project::RecordingTicket>>(ticketResult);
        REQUIRE_FALSE(lease.release());
        CHECK(lease.status().failure == app::RecordingDeviceFailure::busy);
        REQUIRE(std::holds_alternative<std::monostate>(f.owner->cancelRecording(ticket)));
        REQUIRE(lease.release());
        CHECK_FALSE(f.selected->isEnabled());
        CHECK(f.otherSettings() == otherSettings);
        CHECK(synthetic::drivers[0].opens.load() == 1);
    }
    requireNativeCleanup();
}

TEST_CASE("Device lease preserves independent enablement and bounds unpublished preparation")
{
    synthetic::presentInput();
    {
        Fixture f;
        f.selected->setEnabled(false);
        until([&] { return !synthetic::drivers[0].live.load(); });
        app::RecordingDevice lease(*f.owner);
        REQUIRE(lease.select(f.inventory[0].identifier));
        until([&] { REQUIRE(lease.poll()); return lease.ready(); });
        f.selected->setEnabled(true);
        until([&] { return synthetic::drivers[0].live.load(); });
        CHECK_FALSE(lease.current());
        REQUIRE(lease.release());
        CHECK(f.selected->isEnabled());
        CHECK(lease.status().restorationSkipped);
        app::RecordingDevice deadline(*f.owner, 10);
        const auto originalList = f.virtualList();
        REQUIRE(deadline.select(f.inventory[0].identifier));
        // Deliberately withhold the event loop; this is a timeout check, not a
        // fixed-delay device-readiness claim.
        juce::Thread::sleep(15);
        REQUIRE_FALSE(deadline.poll());
        CHECK(deadline.status().failure == app::RecordingDeviceFailure::preparationTimedOut);
        REQUIRE(deadline.release());
        CHECK(f.virtualList() == originalList);
    }
    requireNativeCleanup();
}

TEST_CASE("Device lease accepts a selected MIDI timecode source while synchronization is disabled")
{
    synthetic::presentInput();
    {
        Fixture f;
        auto& edit = f.owner->playbackEdit();
        edit.setCurrentMidiMachineControlSource(nullptr);
        edit.setCurrentMidiTimecodeSource(f.selected);
        edit.enableTimecodeSync(false);
        REQUIRE_FALSE(edit.isTimecodeSyncEnabled());
        REQUIRE(edit.getCurrentMidiTimecodeSource() == f.selected);
        REQUIRE(edit.getCurrentMidiMachineControlSource() == nullptr);
        REQUIRE_FALSE(f.selected->isUsedForExternalControl());
        f.owner->stop();

        const auto originalList = f.virtualList();
        const auto otherSettings = f.otherSettings();
        app::RecordingDevice lease(*f.owner);
        REQUIRE(lease.select(f.inventory[0].identifier));
        until([&] { REQUIRE(lease.poll()); return lease.ready(); });
        REQUIRE(lease.current());
        CHECK_FALSE(f.selected->isEnabled());
        CHECK_FALSE(edit.isTimecodeSyncEnabled());
        CHECK(edit.getCurrentMidiTimecodeSource() == f.selected);
        REQUIRE(lease.release());
        until([&] { return f.selected->isEnabled() && synthetic::drivers[0].live.load(); });
        CHECK(f.virtualList() == originalList);
        CHECK(f.otherSettings() == otherSettings);
        CHECK_FALSE(lease.status().restorationSkipped);
    }
    requireNativeCleanup();
}

TEST_CASE("Device lease rejects active timecode and MIDI machine control sources")
{
    synthetic::presentInput();
    {
        Fixture f;
        auto& edit = f.owner->playbackEdit();
        edit.setCurrentMidiTimecodeSource(f.other);
        edit.enableTimecodeSync(false);
        edit.setCurrentMidiMachineControlSource(nullptr);
        REQUIRE_FALSE(f.selected->isUsedForExternalControl());

        SECTION("Enabled timecode source")
        {
            edit.setCurrentMidiTimecodeSource(f.selected);
            edit.enableTimecodeSync(true);
            REQUIRE(edit.isTimecodeSyncEnabled());
            REQUIRE(edit.getCurrentMidiTimecodeSource() == f.selected);
            REQUIRE(edit.getCurrentMidiMachineControlSource() == nullptr);
        }
        SECTION("MMC remains protected when timecode is disabled")
        {
            edit.setCurrentMidiMachineControlSource(f.selected);
            REQUIRE_FALSE(edit.isTimecodeSyncEnabled());
            REQUIRE(edit.getCurrentMidiTimecodeSource() == f.other);
            REQUIRE(edit.getCurrentMidiMachineControlSource() == f.selected);
        }
        f.owner->stop();

        const auto originalList = f.virtualList();
        const auto otherSettings = f.otherSettings();
        const auto opensBefore = synthetic::drivers[0].opens.load();
        const auto closesBefore = synthetic::drivers[0].closes.load();
        app::RecordingDevice lease(*f.owner);
        REQUIRE_FALSE(lease.select(f.inventory[0].identifier));
        CHECK(lease.status().failure == app::RecordingDeviceFailure::externalControl);
        CHECK(f.selected->isEnabled());
        CHECK(f.other->isEnabled());
        CHECK(synthetic::drivers[0].live.load());
        CHECK(synthetic::drivers[0].opens.load() == opensBefore);
        CHECK(synthetic::drivers[0].closes.load() == closesBefore);
        CHECK(f.virtualList() == originalList);
        CHECK(f.otherSettings() == otherSettings);
        CHECK_FALSE(lease.dedicatedInput());
        REQUIRE(lease.release());
    }
    requireNativeCleanup();
}
