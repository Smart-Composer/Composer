#include "OperatorNewProbe.h"

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
 #include <crtdbg.h>
 #include <malloc.h>
#endif

// The replaceable global allocation functions. They must be defined in exactly one translation
// unit of an executable, so this file belongs only to the test executables that use the probe.

namespace
{

// Constant-initialised, so reading them never needs the thread's dynamic initialisation, which
// could itself allocate.
constinit thread_local bool armed = false;
constinit thread_local long calls = 0;

void count() noexcept
{
    if (armed)
        ++calls;
}

void* allocate(std::size_t size)
{
    count();

    if (size == 0)
        size = 1;

    for (;;)
    {
        if (auto* memory = std::malloc(size))
            return memory;

        if (auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
}

void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    count();

    if (size == 0)
        size = 1;

    for (;;)
    {
       #if defined(_WIN32)
        auto* memory = _aligned_malloc(size, static_cast<std::size_t>(alignment));
       #else
        auto* memory = std::aligned_alloc(static_cast<std::size_t>(alignment),
                                          (size + static_cast<std::size_t>(alignment) - 1) & ~(static_cast<std::size_t>(alignment) - 1));
       #endif

        if (memory != nullptr)
            return memory;

        if (auto handler = std::get_new_handler())
            handler();
        else
            throw std::bad_alloc();
    }
}

void release(void* memory) noexcept
{
    if (memory == nullptr)
        return;

    count();

   #if defined(_WIN32) && defined(_DEBUG)
    // As the runtime's own operator delete frees in Debug builds, whatever the block's type.
    _free_dbg(memory, _UNKNOWN_BLOCK);
   #else
    std::free(memory);
   #endif
}

void releaseAligned(void* memory) noexcept
{
    if (memory == nullptr)
        return;

    count();

   #if defined(_WIN32)
    _aligned_free(memory);
   #else
    std::free(memory);
   #endif
}

} // namespace

void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try
    {
        return allocateAligned(size, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try
    {
        return allocateAligned(size, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete(void* memory) noexcept
{
    release(memory);
}

void operator delete[](void* memory) noexcept
{
    release(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    release(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    release(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept
{
    release(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept
{
    release(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(memory);
}

void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    releaseAligned(memory);
}

void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept
{
    releaseAligned(memory);
}

namespace composer::tests
{

void OperatorNewProbe::arm() noexcept
{
    calls = 0;
    armed = true;
}

long OperatorNewProbe::disarm() noexcept
{
    armed = false;
    return calls;
}

} // namespace composer::tests
