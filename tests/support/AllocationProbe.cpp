#include "AllocationProbe.h"

#if defined(_WIN32) && defined(_DEBUG)
 #include <crtdbg.h>
 #include <cstddef>
 #include <mutex>
#endif

namespace composer::tests
{

#if defined(_WIN32) && defined(_DEBUG)

namespace
{

thread_local bool armed = false;
thread_local long count = 0;

int countingHook(int allocationType, void*, std::size_t, int, long, const unsigned char*, int)
{
    if (armed && (allocationType == _HOOK_ALLOC || allocationType == _HOOK_REALLOC || allocationType == _HOOK_FREE))
        ++count;

    return 1;
}

void installHook()
{
    static std::once_flag installed;
    std::call_once(installed, [] { _CrtSetAllocHook(countingHook); });
}

} // namespace

bool AllocationProbe::available() noexcept
{
    return true;
}

void AllocationProbe::arm() noexcept
{
    installHook();
    count = 0;
    armed = true;
}

long AllocationProbe::disarm() noexcept
{
    armed = false;
    return count;
}

#else

bool AllocationProbe::available() noexcept
{
    return false;
}

void AllocationProbe::arm() noexcept
{
}

long AllocationProbe::disarm() noexcept
{
    return 0;
}

#endif

} // namespace composer::tests
