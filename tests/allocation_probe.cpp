#include "allocation_probe.hpp"
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <malloc.h>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif
#endif

namespace {
#if defined(_WIN32)
// Unattended suites (docs/session.md, "Staying awake"): every test process that links the probe (the demo, every unit test)
// keeps the machine and the display out of idle standby while it runs, from before main, so an overnight CTest run is not put
// to sleep inside a long test (a Debug run once slept 20 minutes inside media_acoustic_intent_tests). On exit the idle
// countdown starts again from there. Between the processes, media_stay_awake holds the same for the whole suite.
struct StayAwake {
    StayAwake() noexcept { (void)SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); }
    ~StayAwake() { (void)SetThreadExecutionState(ES_CONTINUOUS); (void)SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED); }
} stay_awake;
#endif
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> allocations{0};
void Record() noexcept { if (enabled.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed); }
#if defined(_MSC_VER) && defined(_DEBUG)
int Hook(int type, void*, std::size_t, int, long, const unsigned char*, int) {
    if (type == _HOOK_ALLOC || type == _HOOK_REALLOC) Record();
    return 1;
}
_CRT_ALLOC_HOOK previous_hook = nullptr;
#endif
void* Allocate(std::size_t size) {
    Record();
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc{};
}
void* Aligned(std::size_t size, std::size_t alignment) {
    Record();
    if (!size) size = 1;
#if defined(_WIN32)
    if (void* p = _aligned_malloc(size, alignment)) return p;
#else
    if (size <= (std::numeric_limits<std::size_t>::max)() - (alignment - 1))
        if (void* p = std::aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1))) return p;
#endif
    throw std::bad_alloc{};
}
void FreeAligned(void* p) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}
namespace allocation_probe {
void Begin() noexcept {
    allocations.store(0);
#if defined(_MSC_VER) && defined(_DEBUG)
    previous_hook = _CrtSetAllocHook(Hook);
#endif
    enabled.store(true);
}
std::uint64_t End() noexcept {
    enabled.store(false);
#if defined(_MSC_VER) && defined(_DEBUG)
    (void)_CrtSetAllocHook(previous_hook);
#endif
    return allocations.load();
}
}
void* operator new(std::size_t n) { return Allocate(n); }
void* operator new[](std::size_t n) { return Allocate(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) { return Aligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return Aligned(n, static_cast<std::size_t>(a)); }
void operator delete(void* p, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { FreeAligned(p); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return Allocate(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return Allocate(n); } catch (...) { return nullptr; } }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return Aligned(n, static_cast<std::size_t>(a)); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return Aligned(n, static_cast<std::size_t>(a)); } catch (...) { return nullptr; }
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { FreeAligned(p); }
