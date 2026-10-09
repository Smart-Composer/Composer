#pragma once

namespace composer::tests
{

/** Counts heap allocations, reallocations and frees made by the current thread while armed.

    It uses the debug C runtime's allocation hook, which sees malloc as well as operator new, so it
    is available only in Debug builds.
*/
class AllocationProbe
{
public:
    static bool available() noexcept;

    /** Starts counting on this thread from zero. */
    static void arm() noexcept;

    /** Stops counting on this thread and returns the count since arm(). */
    static long disarm() noexcept;
};

} // namespace composer::tests
