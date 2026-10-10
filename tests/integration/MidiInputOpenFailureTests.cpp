// Opens a Windows MIDI input that fails to open, without any MIDI hardware. The winmm functions
// JUCE's input backend calls are redirected, for the life of this process, to stand-ins that
// present one synthetic input and no outputs. JUCE takes some of them as function pointers during
// static initialisation, so the functions themselves are redirected rather than this executable's
// imports. This executable holds nothing else, and CTest runs each test case in its own process,
// so the redirection never reaches another test.

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_devices/juce_audio_devices.h>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>

namespace
{

enum class Failure
{
    open,
    start
};

Failure failure = Failure::open;
const auto syntheticHandle = reinterpret_cast<HMIDIIN>(static_cast<std::uintptr_t>(0x5a5a));
constexpr const wchar_t* syntheticName = L"Composer synthetic input";

UINT WINAPI inputCount() { return 1; }
UINT WINAPI outputCount() { return 0; }

MMRESULT WINAPI inputCaps(UINT_PTR device, LPMIDIINCAPSW caps, UINT size)
{
    if (device != 0 || caps == nullptr || size < sizeof(MIDIINCAPSW))
        return MMSYSERR_BADDEVICEID;

    *caps = {};
    wcsncpy_s(caps->szPname, syntheticName, _TRUNCATE);
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI inputMessage(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR) { return MMSYSERR_NOTSUPPORTED; }

MMRESULT WINAPI openInput(LPHMIDIIN handle, UINT device, DWORD_PTR, DWORD_PTR, DWORD)
{
    // A device that another application holds is refused with MMSYSERR_ALLOCATED.
    if (device != 0 || failure == Failure::open)
        return MMSYSERR_ALLOCATED;

    *handle = syntheticHandle;
    return MMSYSERR_NOERROR;
}

MMRESULT WINAPI acceptHeader(HMIDIIN handle, LPMIDIHDR, UINT)
{
    return handle == syntheticHandle ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

MMRESULT WINAPI startInput(HMIDIIN) { return MMSYSERR_ERROR; }

MMRESULT WINAPI acceptHandle(HMIDIIN handle)
{
    return handle == syntheticHandle ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}

/** Overwrites the start of an exported winmm function with a jump to a stand-in. */
void redirect(const char* name, const void* standIn)
{
    const auto module = GetModuleHandleW(L"winmm.dll");
    REQUIRE(module != nullptr);

    auto* target = reinterpret_cast<std::uint8_t*>(GetProcAddress(module, name));
    REQUIRE(target != nullptr);

    // mov rax, standIn; jmp rax
    std::array<std::uint8_t, 12> jump { 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0 };
    std::memcpy(jump.data() + 2, &standIn, sizeof(standIn));

    DWORD previous = 0;
    REQUIRE(VirtualProtect(target, jump.size(), PAGE_EXECUTE_READWRITE, &previous));
    std::memcpy(target, jump.data(), jump.size());
    REQUIRE(VirtualProtect(target, jump.size(), previous, &previous));
    REQUIRE(FlushInstructionCache(GetCurrentProcess(), target, jump.size()));
}

void presentFailingInput(Failure kind)
{
    static_assert(sizeof(void*) == 8, "the redirection is written for x64");
    failure = kind;

    static const bool redirected = [] {
        redirect("midiInGetNumDevs", reinterpret_cast<const void*>(&inputCount));
        redirect("midiOutGetNumDevs", reinterpret_cast<const void*>(&outputCount));
        redirect("midiInGetDevCapsW", reinterpret_cast<const void*>(&inputCaps));
        redirect("midiInMessage", reinterpret_cast<const void*>(&inputMessage));
        redirect("midiInOpen", reinterpret_cast<const void*>(&openInput));
        redirect("midiInPrepareHeader", reinterpret_cast<const void*>(&acceptHeader));
        redirect("midiInAddBuffer", reinterpret_cast<const void*>(&acceptHeader));
        redirect("midiInUnprepareHeader", reinterpret_cast<const void*>(&acceptHeader));
        redirect("midiInStart", reinterpret_cast<const void*>(&startInput));
        redirect("midiInStop", reinterpret_cast<const void*>(&acceptHandle));
        redirect("midiInReset", reinterpret_cast<const void*>(&acceptHandle));
        redirect("midiInClose", reinterpret_cast<const void*>(&acceptHandle));
        return true;
    }();

    REQUIRE(redirected);
}

class IgnoreMidi final : public juce::MidiInputCallback
{
public:
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override {}
};

void checkOpenFails()
{
    const auto devices = juce::MidiInput::getAvailableDevices();
    REQUIRE(devices.size() == 1);
    CHECK(devices[0].name == juce::String(syntheticName));

    IgnoreMidi callback;
    CHECK(juce::MidiInput::openDevice(devices[0].identifier, &callback) == nullptr);

    // A failed open leaves nothing behind that would stop a later attempt.
    CHECK(juce::MidiInput::openDevice(devices[0].identifier, &callback) == nullptr);
}

} // namespace

TEST_CASE("A MIDI input that another application holds fails to open without ending the process")
{
    presentFailingInput(Failure::open);
    checkOpenFails();
}

TEST_CASE("A MIDI input that opens but cannot start fails to open without ending the process")
{
    presentFailingInput(Failure::start);
    checkOpenFails();
}
