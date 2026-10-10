#pragma once

namespace composer::tests
{

/** Counts calls to the global operator new and operator delete made by the current thread while
    armed, in every build configuration.

    OperatorNewProbe.cpp replaces every replaceable form of the global operator new and delete:
    single and array, aligned and unaligned, throwing and nothrow, and the sized deletes. Only
    executables that compile that file count; its replacements forward to the C runtime's
    allocator. Deletes of a null pointer free nothing and are not counted.

    Memory taken with malloc, realloc or the system heap directly does not pass through operator
    new, so this probe does not see it. AllocationProbe, which hooks the debug C runtime, sees malloc
    too, but only in Debug builds.
*/
class OperatorNewProbe
{
public:
    /** Starts counting on this thread from zero. */
    static void arm() noexcept;

    /** Stops counting on this thread and returns the calls counted since arm(). */
    static long disarm() noexcept;
};

} // namespace composer::tests
