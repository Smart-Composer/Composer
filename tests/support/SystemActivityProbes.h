#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace composer::tests
{

/** Counts the page faults of the whole process from construction, through the system's process
    memory counters.

    Every thread in the process contributes, so the count is an upper bound on what the measured
    code caused: zero proves the code caused none. available() is false where the system does not
    report it.
*/
class PageFaultProbe
{
public:
    PageFaultProbe() noexcept;

    static bool available() noexcept;

    /** Page faults in the process since construction. */
    std::uint64_t count() const noexcept;

private:
    static std::optional<std::uint32_t> processPageFaults() noexcept;

    std::uint32_t start = 0;
};

/** Counts the context switches of the thread that constructs it, from construction.

    The system's per-thread counter comes from its process information query, which lists every
    thread of every process. The probe takes the buffer for that list from the process heap at
    construction, before its first reading, with room to spare, and count() reuses it: count()
    allocates only if the list has outgrown the buffer, and the probe never uses operator new. The
    query writes the buffer, so it can cause page faults of its own. count() may be called on any
    thread. available() is false where the system does not report the counter.
*/
class ContextSwitchProbe
{
public:
    ContextSwitchProbe() noexcept;
    ~ContextSwitchProbe();

    ContextSwitchProbe(const ContextSwitchProbe&) = delete;
    ContextSwitchProbe& operator=(const ContextSwitchProbe&) = delete;

    static bool available() noexcept;

    /** The constructing thread's context switches since construction, or nothing if a query
        failed. */
    std::optional<std::uint64_t> count() noexcept;

private:
    std::optional<std::uint32_t> threadContextSwitches() noexcept;
    bool resize(std::size_t bytes) noexcept;

    void* buffer = nullptr;
    std::size_t bufferSize = 0;
    std::uint32_t threadId = 0;
    std::optional<std::uint32_t> start;
};

} // namespace composer::tests
