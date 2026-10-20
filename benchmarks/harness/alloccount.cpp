// Counting the allocator, for the runs that explain a result rather than
// time it:
//
//   LD_PRELOAD=libwtc_alloccount.so w1_throughput --variant shim_spsc ...
//
// Every call into malloc, calloc, realloc, the aligned allocators and free
// is counted and passed on to glibc's own entry points (__libc_*), which do
// not come back into this file. operator new and delete reach malloc and
// free through libstdc++, aligned new through aligned_alloc, so they are
// counted too. The harness finds wtc_alloc_counts at run time and writes the
// totals into the results line; without the preload nothing changes.
//
// The counters are atomics every allocating thread touches, so counting
// perturbs the timing it would explain: runs with the preload are kept apart
// from the timed ones.

#include <atomic>
#include <cerrno>
#include <cstddef>

extern "C" {
void* __libc_malloc(std::size_t);
void __libc_free(void*);
void* __libc_calloc(std::size_t, std::size_t);
void* __libc_realloc(void*, std::size_t);
void* __libc_memalign(std::size_t, std::size_t);
}

namespace {

std::atomic<unsigned long> g_allocs{0}, g_frees{0}, g_bytes{0};

void count_alloc(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(n, std::memory_order_relaxed);
}

void count_free() { g_frees.fetch_add(1, std::memory_order_relaxed); }

}

extern "C" {

void* malloc(std::size_t n) {
    count_alloc(n);
    return __libc_malloc(n);
}

void free(void* p) {
    if (p) count_free();
    __libc_free(p);
}

void* calloc(std::size_t k, std::size_t n) {
    count_alloc(k * n);
    return __libc_calloc(k, n);
}

// A resize is a new block as far as the cost goes: one allocation and one
// free, whether glibc moves it or not.
void* realloc(void* p, std::size_t n) {
    if (!p) count_alloc(n);
    else if (n == 0) count_free();
    else { count_alloc(n); count_free(); }
    return __libc_realloc(p, n);
}

void* memalign(std::size_t align, std::size_t n) {
    count_alloc(n);
    return __libc_memalign(align, n);
}

void* aligned_alloc(std::size_t align, std::size_t n) {
    count_alloc(n);
    return __libc_memalign(align, n);
}

int posix_memalign(void** out, std::size_t align, std::size_t n) {
    if (align % sizeof(void*) != 0 || (align & (align - 1)) != 0) return EINVAL;
    void* p = __libc_memalign(align, n);
    if (!p) return ENOMEM;
    count_alloc(n);
    *out = p;
    return 0;
}

void wtc_alloc_counts(unsigned long* allocs, unsigned long* frees, unsigned long* bytes) {
    *allocs = g_allocs.load(std::memory_order_relaxed);
    *frees = g_frees.load(std::memory_order_relaxed);
    *bytes = g_bytes.load(std::memory_order_relaxed);
}

}
