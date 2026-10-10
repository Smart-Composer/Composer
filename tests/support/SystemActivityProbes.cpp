#include "SystemActivityProbes.h"

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <psapi.h>
 #include <winternl.h>

 #include <algorithm>
#endif

namespace composer::tests
{

#if defined(_WIN32)

namespace
{

// NtQuerySystemInformation with SystemProcessInformation fills a buffer with one
// SYSTEM_PROCESS_INFORMATION per process, each followed directly by one SYSTEM_THREAD_INFORMATION
// per thread of that process; both are declared in the Windows SDK's winternl.h. The thread
// entry's Reserved3 is its context-switch count. Microsoft does not document that meaning, but it
// is the field's established one, and the probe's own tests check it.
constexpr LONG statusInfoLengthMismatch = static_cast<LONG>(0xC0000004L);

static_assert(sizeof(void*) == 8, "The entry sizes below are those of 64-bit Windows");
static_assert(sizeof(SYSTEM_PROCESS_INFORMATION) == 0x100, "Thread entries start 0x100 bytes after their process entry");
static_assert(sizeof(SYSTEM_THREAD_INFORMATION) == 0x50, "Thread entries are 0x50 bytes apart");

using QuerySystemInformation = LONG(NTAPI*)(SYSTEM_INFORMATION_CLASS, void*, ULONG, ULONG*);

QuerySystemInformation querySystemInformation() noexcept
{
    static const auto function = [] {
        const auto module = GetModuleHandleW(L"ntdll.dll");
        return module != nullptr
                   ? reinterpret_cast<QuerySystemInformation>(
                         reinterpret_cast<void*>(GetProcAddress(module, "NtQuerySystemInformation")))
                   : nullptr;
    }();

    return function;
}

} // namespace

PageFaultProbe::PageFaultProbe() noexcept
    : start(processPageFaults().value_or(0))
{
}

bool PageFaultProbe::available() noexcept
{
    return processPageFaults().has_value();
}

std::uint64_t PageFaultProbe::count() const noexcept
{
    // The system counter is 32 bits wide; unsigned subtraction keeps the difference across a wrap.
    return static_cast<std::uint32_t>(processPageFaults().value_or(start) - start);
}

std::optional<std::uint32_t> PageFaultProbe::processPageFaults() noexcept
{
    PROCESS_MEMORY_COUNTERS counters {};
    counters.cb = sizeof(counters);

    if (! GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return std::nullopt;

    return static_cast<std::uint32_t>(counters.PageFaultCount);
}

ContextSwitchProbe::ContextSwitchProbe() noexcept
    : threadId(static_cast<std::uint32_t>(GetCurrentThreadId()))
{
    const auto query = querySystemInformation();

    if (query == nullptr)
        return;

    // A query into an empty buffer reports the size the list needs now; twice that leaves room
    // for processes and threads that start later.
    ULONG required = 0;
    query(SystemProcessInformation, nullptr, 0, &required);

    if (resize(std::max<std::size_t>(2 * static_cast<std::size_t>(required), 1 << 20)))
        start = threadContextSwitches();
}

ContextSwitchProbe::~ContextSwitchProbe()
{
    resize(0);
}

bool ContextSwitchProbe::available() noexcept
{
    ContextSwitchProbe probe;
    return probe.start.has_value();
}

std::optional<std::uint64_t> ContextSwitchProbe::count() noexcept
{
    const auto now = threadContextSwitches();

    if (! start.has_value() || ! now.has_value())
        return std::nullopt;

    // The system counter is 32 bits wide; unsigned subtraction keeps the difference across a wrap.
    return static_cast<std::uint32_t>(*now - *start);
}

bool ContextSwitchProbe::resize(std::size_t bytes) noexcept
{
    if (buffer != nullptr)
        HeapFree(GetProcessHeap(), 0, buffer);

    // The process heap, so that the probe never goes through operator new.
    buffer = bytes > 0 ? HeapAlloc(GetProcessHeap(), 0, bytes) : nullptr;
    bufferSize = buffer != nullptr ? bytes : 0;
    return buffer != nullptr;
}

std::optional<std::uint32_t> ContextSwitchProbe::threadContextSwitches() noexcept
{
    const auto query = querySystemInformation();

    if (query == nullptr)
        return std::nullopt;

    const auto processId = static_cast<std::uintptr_t>(GetCurrentProcessId());

    for (int attempt = 0; attempt < 8 && buffer != nullptr; ++attempt)
    {
        ULONG required = 0;
        const auto status = query(SystemProcessInformation, buffer, static_cast<ULONG>(bufferSize), &required);

        // The list outgrew the buffer: grow it, which allocates inside the measured interval.
        if (status == statusInfoLengthMismatch)
        {
            if (! resize(std::max<std::size_t>(2 * bufferSize, static_cast<std::size_t>(required) + (1 << 16))))
                return std::nullopt;

            continue;
        }

        if (status < 0)
            return std::nullopt;

        const auto* entry = static_cast<const std::byte*>(buffer);

        for (;;)
        {
            const auto* process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(entry);

            if (reinterpret_cast<std::uintptr_t>(process->UniqueProcessId) == processId)
            {
                const auto* threads = reinterpret_cast<const SYSTEM_THREAD_INFORMATION*>(process + 1);

                for (ULONG index = 0; index < process->NumberOfThreads; ++index)
                    if (reinterpret_cast<std::uintptr_t>(threads[index].ClientId.UniqueThread) == threadId)
                        return static_cast<std::uint32_t>(threads[index].Reserved3);

                return std::nullopt;
            }

            if (process->NextEntryOffset == 0)
                return std::nullopt;

            entry += process->NextEntryOffset;
        }
    }

    return std::nullopt;
}

#else

PageFaultProbe::PageFaultProbe() noexcept = default;

bool PageFaultProbe::available() noexcept
{
    return false;
}

std::uint64_t PageFaultProbe::count() const noexcept
{
    return 0;
}

std::optional<std::uint32_t> PageFaultProbe::processPageFaults() noexcept
{
    return std::nullopt;
}

ContextSwitchProbe::ContextSwitchProbe() noexcept = default;

ContextSwitchProbe::~ContextSwitchProbe() = default;

bool ContextSwitchProbe::available() noexcept
{
    return false;
}

std::optional<std::uint64_t> ContextSwitchProbe::count() noexcept
{
    return std::nullopt;
}

std::optional<std::uint32_t> ContextSwitchProbe::threadContextSwitches() noexcept
{
    return std::nullopt;
}

bool ContextSwitchProbe::resize(std::size_t) noexcept
{
    return false;
}

#endif

} // namespace composer::tests
