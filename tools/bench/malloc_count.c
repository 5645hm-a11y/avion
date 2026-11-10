/* Counts every heap allocation of a process (LD_PRELOAD; Linux, glibc). The head-to-head benchmark (docs/benchmarks.md) loads
 * it into AVION's harness, ffmpeg and the OpenCV baseline alike, so all three are counted by the same rule: each call of
 * malloc, calloc, realloc, aligned_alloc, posix_memalign, memalign, valloc and pvalloc is one allocation (C++'s operator new
 * calls malloc). Programs that know of it read the count with bench_alloc_count(); with MALLOC_COUNT_REPORT set, the total is
 * printed to stderr at exit (programs that do not, such as ffmpeg: measured by differencing two run lengths).
 *   gcc -O2 -shared -fPIC tools/bench/malloc_count.c -o malloc_count.so */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

extern void* __libc_malloc(size_t);
extern void* __libc_calloc(size_t, size_t);
extern void* __libc_realloc(void*, size_t);
extern void* __libc_memalign(size_t, size_t);

static atomic_ullong allocations;
static inline void Count(void) { atomic_fetch_add_explicit(&allocations, 1, memory_order_relaxed); }

unsigned long long bench_alloc_count(void) { return atomic_load_explicit(&allocations, memory_order_relaxed); }

void* malloc(size_t n) { Count(); return __libc_malloc(n); }
void* calloc(size_t n, size_t s) { Count(); return __libc_calloc(n, s); }
void* realloc(void* p, size_t n) { Count(); return __libc_realloc(p, n); }
void* memalign(size_t a, size_t n) { Count(); return __libc_memalign(a, n); }
void* aligned_alloc(size_t a, size_t n) { Count(); return __libc_memalign(a, n); }
void* valloc(size_t n) { Count(); return __libc_memalign(4096, n); }
void* pvalloc(size_t n) { Count(); return __libc_memalign(4096, (n + 4095) & ~(size_t)4095); }
int posix_memalign(void** out, size_t a, size_t n) {
    Count();
    void* p = __libc_memalign(a, n);
    if (!p) return 12; /* ENOMEM */
    *out = p;
    return 0;
}

__attribute__((destructor)) static void Report(void) {
    if (getenv("MALLOC_COUNT_REPORT")) fprintf(stderr, "malloc_count total=%llu\n", bench_alloc_count());
}
