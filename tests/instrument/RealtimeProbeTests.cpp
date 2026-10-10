#include "OperatorNewProbe.h"
#include "SystemActivityProbes.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <new>
#include <optional>
#include <thread>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

// Each probe must see the event it exists to detect; a probe that cannot would let the real-time
// tests pass vacuously.

namespace
{

using namespace composer::tests;

void* touch(void* memory)
{
    if (memory != nullptr)
        static_cast<volatile unsigned char*>(memory)[0] = 1;

    return memory;
}

/** The calls one round of allocateEveryWay makes: twelve allocations and twelve deletes. */
constexpr long callsPerRound = 24;

/** Calls each of the twenty replaceable allocation and deallocation functions at least once, with
    one allocation for each form of delete. */
void allocateEveryWay()
{
    constexpr std::size_t size = 96;
    constexpr auto alignment = std::align_val_t { 64 };

    ::operator delete(touch(::operator new(size)));
    ::operator delete(touch(::operator new(size)), size);
    ::operator delete[](touch(::operator new[](size)));
    ::operator delete[](touch(::operator new[](size)), size);
    ::operator delete(touch(::operator new(size, std::nothrow)), std::nothrow);
    ::operator delete[](touch(::operator new[](size, std::nothrow)), std::nothrow);

    ::operator delete(touch(::operator new(size, alignment)), alignment);
    ::operator delete(touch(::operator new(size, alignment)), size, alignment);
    ::operator delete[](touch(::operator new[](size, alignment)), alignment);
    ::operator delete[](touch(::operator new[](size, alignment)), size, alignment);
    ::operator delete(touch(::operator new(size, alignment, std::nothrow)), alignment, std::nothrow);
    ::operator delete[](touch(::operator new[](size, alignment, std::nothrow)), alignment, std::nothrow);
}

} // namespace

TEST_CASE("The operator new probe counts new and delete on its own thread only")
{
    OperatorNewProbe::arm();
    allocateEveryWay();
    CHECK(OperatorNewProbe::disarm() == callsPerRound);

    // Another thread allocates, arms, allocates and disarms while this one is armed and waits
    // without allocating; then this one allocates. A count shared between threads, or an armed
    // state that the other thread's disarm turns off, gives this thread a different count.
    std::atomic<int> stage { 0 };
    long otherThreadCount = 0;

    std::thread other([&] {
        while (stage.load() != 1)
            std::this_thread::yield();

        allocateEveryWay();

        OperatorNewProbe::arm();
        allocateEveryWay();
        otherThreadCount = OperatorNewProbe::disarm();

        stage.store(2);
    });

    OperatorNewProbe::arm();
    stage.store(1);

    while (stage.load() != 2)
        std::this_thread::yield();

    allocateEveryWay();
    const long thisThreadCount = OperatorNewProbe::disarm();
    other.join();

    CHECK(thisThreadCount == callsPerRound);
    CHECK(otherThreadCount == callsPerRound);
}

TEST_CASE("The page fault probe counts the first touch of freshly committed memory")
{
#if defined(_WIN32)
    REQUIRE(PageFaultProbe::available());

    SYSTEM_INFO system {};
    GetSystemInfo(&system);
    const std::size_t pageSize = system.dwPageSize;
    constexpr std::size_t bytes = 32u << 20;
    const std::size_t pages = bytes / pageSize;

    auto* region = static_cast<volatile unsigned char*>(VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    REQUIRE(region != nullptr);

    const PageFaultProbe probe;

    for (std::size_t offset = 0; offset < bytes; offset += pageSize)
        region[offset] = 1;

    const auto faults = probe.count();
    VirtualFree(const_cast<unsigned char*>(region), 0, MEM_RELEASE);

    // Committed memory gets its pages on first touch; the system may map a few at a time.
    INFO(faults << " page faults for " << pages << " pages");
    CHECK(faults >= pages / 8);
#else
    SKIP("Page fault counting is implemented for Windows.");
#endif
}

TEST_CASE("The context switch probe counts a wait on a mutex another thread holds")
{
#if defined(_WIN32)
    REQUIRE(ContextSwitchProbe::available());

    // Measured on a thread of its own rather than the process's first thread, which a probe that
    // ignored the thread ID would read instead.
    std::optional<std::uint64_t> switches;

    std::thread measured([&switches] {
        std::mutex mutex;
        std::atomic<bool> held { false };

        std::thread holder([&] {
            const std::lock_guard lock(mutex);
            held.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });

        while (! held.load())
            std::this_thread::yield();

        ContextSwitchProbe probe;

        {
            const std::lock_guard lock(mutex);
        }

        switches = probe.count();
        holder.join();
    });

    measured.join();

    REQUIRE(switches.has_value());
    CHECK(*switches >= 1);
#else
    SKIP("Context switch counting is implemented for Windows.");
#endif
}

TEST_CASE("The context switch probe counts only its own thread")
{
#if defined(_WIN32)
    REQUIRE(ContextSwitchProbe::available());

    // The measured thread blocks once while this thread and another pass control back and forth
    // two thousand times, each pass a wait on an event. A count of the whole process, or of
    // another thread, would include those thousands of switches.
    constexpr int passes = 2000;
    const auto event = [] { return CreateEventW(nullptr, FALSE, FALSE, nullptr); };
    const HANDLE measuredMayFinish = event();
    const HANDLE partnerTurn = event();
    const HANDLE ownTurn = event();
    REQUIRE((measuredMayFinish != nullptr && partnerTurn != nullptr && ownTurn != nullptr));

    std::atomic<bool> measuring { false };
    std::optional<std::uint64_t> measuredSwitches;

    std::thread measured([&] {
        ContextSwitchProbe probe;
        measuring.store(true);
        WaitForSingleObject(measuredMayFinish, INFINITE);
        measuredSwitches = probe.count();
    });

    std::thread partner([&] {
        for (int pass = 0; pass < passes; ++pass)
        {
            WaitForSingleObject(partnerTurn, INFINITE);
            SetEvent(ownTurn);
        }
    });

    while (! measuring.load())
        std::this_thread::yield();

    ContextSwitchProbe ownProbe;

    for (int pass = 0; pass < passes; ++pass)
    {
        SetEvent(partnerTurn);
        WaitForSingleObject(ownTurn, INFINITE);
    }

    const auto ownSwitches = ownProbe.count();
    partner.join();
    SetEvent(measuredMayFinish);
    measured.join();

    for (const auto handle : { measuredMayFinish, partnerTurn, ownTurn })
        CloseHandle(handle);

    // This thread's own count shows the passes switched; the measured thread's must not.
    REQUIRE(ownSwitches.has_value());
    REQUIRE(measuredSwitches.has_value());
    INFO("this thread " << *ownSwitches << ", the measured thread " << *measuredSwitches);
    CHECK(*ownSwitches >= passes / 2);
    CHECK(*measuredSwitches >= 1);
    CHECK(*measuredSwitches < passes / 2);
#else
    SKIP("Context switch counting is implemented for Windows.");
#endif
}
