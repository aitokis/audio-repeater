#include "allocator_hooks.h"
#include <atomic>
#include <new>
#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace {
std::atomic<bool> tracking{false};
std::atomic<std::size_t> allocations{0};
void count() noexcept { if (tracking.load(std::memory_order_relaxed)) ++allocations; }
}
extern "C" {
void audio_test_track(int enabled) { allocations = 0; tracking = enabled != 0; }
size_t audio_test_allocations() { return allocations.load(); }
void* audio_test_malloc(size_t n) { count(); return std::malloc(n); }
void* audio_test_calloc(size_t n, size_t s) { count(); return std::calloc(n, s); }
void* audio_test_realloc(void* p, size_t n) { count(); return std::realloc(p, n); }
void audio_test_free(void* p) { std::free(p); }
}
void* operator new(std::size_t n) {
    if (auto* p = audio_test_malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t alignment) {
    count();
    const auto a = static_cast<std::size_t>(alignment);
#ifdef _MSC_VER
    auto* p = _aligned_malloc(n == 0 ? a : n, a);
#else
    auto* p = std::aligned_alloc(a, ((n == 0 ? 1 : n) + a - 1) / a * a);
#endif
    if (p) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept {
#ifdef _MSC_VER
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
