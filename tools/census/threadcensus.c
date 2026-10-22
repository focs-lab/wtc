/* threadcensus: which threads take which mutex, counted at run time.
 *
 * A preloaded library (LD_PRELOAD=libwtc_threadcensus.so) that interposes
 * pthread_mutex_lock, pthread_mutex_trylock and pthread_mutex_unlock, the
 * calls behind std::mutex and std::lock_guard as well as the C API, and
 * keeps, for every mutex it sees, the set of threads that acquired it and
 * how many times. At exit it writes a report to the file named by
 * WTC_CENSUS_OUT (stderr when unset): what share of all acquisitions fell
 * on mutexes only one thread ever took, and the twenty busiest mutexes with
 * their thread count and the call stack of their first acquisition.
 *
 * The question it answers is how much of a program's locking a compiler
 * could remove by proving the mutex confined to one thread, which is the
 * ceiling for that transformation before any analysis is written. A mutex
 * taken by one thread is not necessarily confinable (the second thread may
 * come later in a longer run), so the number is an upper bound measured on
 * the given input.
 *
 * Plain C, so it loads under C programs; no allocation after start-up on
 * the locking path; a fixed open-addressing table, so a program with more
 * mutexes than it holds reports the overflow and the share stays honest.
 * Threads are told apart by pthread_self; a thread id reused after a join
 * counts as the old thread, which errs towards "one thread".
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TABLE_BITS 16
#define TABLE_SIZE (1u << TABLE_BITS)
#define MAX_OWNERS 8      /* distinct threads remembered per mutex */
#define STACK_DEPTH 12

struct entry {
    _Atomic(uintptr_t) key;            /* the mutex's address, 0 when free */
    _Atomic(unsigned long) locks;      /* acquisitions */
    _Atomic(unsigned) nowners;         /* distinct threads, MAX_OWNERS+1 means more */
    _Atomic(uintptr_t) owners[MAX_OWNERS];
    void* stack[STACK_DEPTH];          /* the first acquisition's call stack */
    int depth;
    _Atomic(int) stack_state;          /* 0 empty, 1 being written, 2 ready */
};

static struct entry table[TABLE_SIZE];
static _Atomic(unsigned long) overflow;   /* acquisitions of mutexes the table could not hold */
static _Atomic(unsigned long) total;

static int (*real_lock)(pthread_mutex_t*);
static int (*real_trylock)(pthread_mutex_t*);
static int (*real_unlock)(pthread_mutex_t*);
static __thread int in_hook;   /* backtrace may lock; no counting under it */

static void resolve(void) {
    if (real_lock) return;
    real_lock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_trylock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_unlock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_unlock");
}

static struct entry* find(uintptr_t key) {
    uintptr_t h = (key >> 4) * 0x9E3779B97F4A7C15ull;
    for (unsigned probe = 0; probe < TABLE_SIZE; ++probe) {
        struct entry* e = &table[((h >> (64 - TABLE_BITS)) + probe) & (TABLE_SIZE - 1)];
        uintptr_t k = atomic_load_explicit(&e->key, memory_order_acquire);
        if (k == key) return e;
        if (k == 0) {
            uintptr_t expect = 0;
            if (atomic_compare_exchange_strong(&e->key, &expect, key)) return e;
            if (expect == key) return e;
        }
    }
    return NULL;
}

static void note(pthread_mutex_t* m) {
    atomic_fetch_add_explicit(&total, 1, memory_order_relaxed);
    struct entry* e = find((uintptr_t)m);
    if (!e) { atomic_fetch_add_explicit(&overflow, 1, memory_order_relaxed); return; }
    atomic_fetch_add_explicit(&e->locks, 1, memory_order_relaxed);
    uintptr_t me = (uintptr_t)pthread_self();
    unsigned n = atomic_load_explicit(&e->nowners, memory_order_acquire);
    for (unsigned i = 0; i < n && i < MAX_OWNERS; ++i)
        if (atomic_load_explicit(&e->owners[i], memory_order_relaxed) == me) goto stack;
    /* A thread not yet seen on this mutex: claim the next slot. */
    for (;;) {
        n = atomic_load_explicit(&e->nowners, memory_order_acquire);
        if (n > MAX_OWNERS) break;
        if (n == MAX_OWNERS) { atomic_store(&e->nowners, MAX_OWNERS + 1); break; }
        uintptr_t expect = 0;
        if (atomic_compare_exchange_strong(&e->owners[n], &expect, me)) {
            atomic_store_explicit(&e->nowners, n + 1, memory_order_release);
            break;
        }
        if (expect == me) break;
        /* another thread took slot n; it will bump nowners, retry */
        while (atomic_load_explicit(&e->nowners, memory_order_acquire) == n) sched_yield();
    }
stack:;
    int s = 0;
    if (atomic_load_explicit(&e->stack_state, memory_order_acquire) == 0 &&
        atomic_compare_exchange_strong(&e->stack_state, &s, 1)) {
        in_hook = 1;
        e->depth = backtrace(e->stack, STACK_DEPTH);
        in_hook = 0;
        atomic_store_explicit(&e->stack_state, 2, memory_order_release);
    }
}

int pthread_mutex_lock(pthread_mutex_t* m) {
    resolve();
    int rc = real_lock(m);
    if (rc == 0 && !in_hook) note(m);
    return rc;
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
    resolve();
    int rc = real_trylock(m);
    if (rc == 0 && !in_hook) note(m);
    return rc;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
    resolve();
    return real_unlock(m);
}

static int by_locks(const void* a, const void* b) {
    unsigned long x = atomic_load(&(*(struct entry* const*)a)->locks);
    unsigned long y = atomic_load(&(*(struct entry* const*)b)->locks);
    return x < y ? 1 : x > y ? -1 : 0;
}

__attribute__((destructor)) static void report(void) {
    const char* path = getenv("WTC_CENSUS_OUT");
    FILE* out = path ? fopen(path, "w") : stderr;
    if (!out) out = stderr;
    static struct entry* seen[TABLE_SIZE];
    unsigned nseen = 0;
    unsigned long locks_total = 0, locks_single = 0, locks_many_unknown = 0;
    unsigned mutexes = 0, single = 0;
    for (unsigned i = 0; i < TABLE_SIZE; ++i) {
        struct entry* e = &table[i];
        if (atomic_load(&e->key) == 0) continue;
        unsigned long l = atomic_load(&e->locks);
        unsigned n = atomic_load(&e->nowners);
        ++mutexes; locks_total += l;
        if (n == 1) { ++single; locks_single += l; }
        if (n > MAX_OWNERS) locks_many_unknown += l;
        seen[nseen++] = e;
    }
    unsigned long over = atomic_load(&overflow);
    fprintf(out, "threadcensus: %u mutexes, %lu acquisitions (%lu on mutexes the table could not hold)\n",
            mutexes, locks_total + over, over);
    fprintf(out, "single-thread mutexes: %u of %u; their acquisitions: %lu of %lu (%.1f%%)\n",
            single, mutexes, locks_single, locks_total + over,
            locks_total + over ? 100.0 * (double)locks_single / (double)(locks_total + over) : 0.0);
    if (locks_many_unknown)
        fprintf(out, "acquisitions on mutexes with more than %d threads (count not kept): %lu\n",
                MAX_OWNERS, locks_many_unknown);
    qsort(seen, nseen, sizeof seen[0], by_locks);
    fprintf(out, "\ntop %u by acquisitions (address, acquisitions, threads, first acquisition's stack):\n",
            nseen < 20 ? nseen : 20);
    fflush(out);
    for (unsigned i = 0; i < nseen && i < 20; ++i) {
        struct entry* e = seen[i];
        unsigned n = atomic_load(&e->nowners);
        fprintf(out, "\n%#lx  %lu  %s%u\n", (unsigned long)atomic_load(&e->key), atomic_load(&e->locks),
                n > MAX_OWNERS ? ">" : "", n > MAX_OWNERS ? MAX_OWNERS : n);
        fflush(out);
        if (atomic_load(&e->stack_state) == 2 && e->depth > 2)
            backtrace_symbols_fd(e->stack + 2, e->depth - 2, fileno(out));
    }
    if (out != stderr) fclose(out);
}
