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
 * the locking path; an open-addressing table of WTC_CENSUS_TABLE entries
 * (a power of two, 2^20 by default, mapped at start and touched as used),
 * so a program with more mutexes than it holds reports the overflow and
 * the share stays honest. A destroyed mutex closes its entry, so another
 * mutex at the same address later starts an entry of its own. The
 * re-acquisition inside pthread_cond_wait and pthread_cond_timedwait is
 * counted as an acquisition, since it is one. Threads are told apart by
 * pthread_self; a thread id reused after a join counts as the old thread,
 * which errs towards "one thread". Not seen: rwlocks, spinlocks, timed and
 * clock locks, and anything built on atomics.
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
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define TABLE_BITS_DEFAULT 20
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
    _Atomic(int) closed;               /* the mutex was destroyed; the address may be reused */
};

static struct entry* table;
static unsigned table_bits = TABLE_BITS_DEFAULT;
static unsigned long table_size;
static _Atomic(unsigned long) overflow;   /* acquisitions of mutexes the table could not hold */

static int (*real_lock)(pthread_mutex_t*);
static int (*real_trylock)(pthread_mutex_t*);
static int (*real_unlock)(pthread_mutex_t*);
static int (*real_destroy)(pthread_mutex_t*);
static int (*real_cond_wait)(pthread_cond_t*, pthread_mutex_t*);
static int (*real_cond_timedwait)(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
static _Atomic(int) ready;     /* every symbol resolved and the table mapped */
static __thread int in_hook;   /* backtrace may lock; no counting under it */

/* Resolves every symbol and maps the table, once, before any thread but
 * the first can run (the constructor runs at load); a call that arrives
 * earlier still, from the loader, resolves on its own and counts nothing
 * until the table is there. */
static void setup(void) {
    if (atomic_load_explicit(&ready, memory_order_acquire)) return;
    real_lock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_trylock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_unlock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_destroy = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    real_cond_wait = (int (*)(pthread_cond_t*, pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_cond_wait");
    real_cond_timedwait = (int (*)(pthread_cond_t*, pthread_mutex_t*, const struct timespec*))
        dlsym(RTLD_NEXT, "pthread_cond_timedwait");
    const char* bits = getenv("WTC_CENSUS_TABLE");
    if (bits) {
        unsigned long n = strtoul(bits, NULL, 10);
        unsigned b = 0;
        while ((1ul << b) < n && b < 30) ++b;
        if (b >= 8) table_bits = b;
    }
    table_size = 1ul << table_bits;
    void* p = mmap(NULL, table_size * sizeof(struct entry), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) table = (struct entry*)p;
    atomic_store_explicit(&ready, 1, memory_order_release);
}
__attribute__((constructor)) static void init(void) { setup(); }

static struct entry* find(uintptr_t key) {
    if (!table) return NULL;
    uintptr_t h = (key >> 4) * 0x9E3779B97F4A7C15ull;
    for (unsigned long probe = 0; probe < table_size; ++probe) {
        struct entry* e = &table[((h >> (64 - table_bits)) + probe) & (table_size - 1)];
        uintptr_t k = atomic_load_explicit(&e->key, memory_order_acquire);
        if (k == key && !atomic_load_explicit(&e->closed, memory_order_acquire)) return e;
        if (k == 0) {
            uintptr_t expect = 0;
            if (atomic_compare_exchange_strong(&e->key, &expect, key)) return e;
            if (expect == key && !atomic_load_explicit(&e->closed, memory_order_acquire)) return e;
        }
    }
    return NULL;
}

static void note(pthread_mutex_t* m) {
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
    setup();
    int rc = real_lock(m);
    if (rc == 0 && !in_hook) note(m);
    return rc;
}

int pthread_mutex_trylock(pthread_mutex_t* m) {
    setup();
    int rc = real_trylock(m);
    if (rc == 0 && !in_hook) note(m);
    return rc;
}

int pthread_mutex_unlock(pthread_mutex_t* m) {
    setup();
    return real_unlock(m);
}

/* A destroyed mutex's entry is closed: its counts stay in the report, and
 * a mutex that later lives at the same address starts an entry of its own
 * rather than merging into it. */
int pthread_mutex_destroy(pthread_mutex_t* m) {
    setup();
    struct entry* e = find((uintptr_t)m);
    if (e) atomic_store_explicit(&e->closed, 1, memory_order_release);
    return real_destroy(m);
}

/* Waiting on a condition variable gives the mutex up and takes it again
 * inside libc, without passing through the interposed lock; the second
 * acquisition is counted here, by the same thread. */
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    setup();
    int rc = real_cond_wait(c, m);
    if (!in_hook) note(m);
    return rc;
}

int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* t) {
    setup();
    int rc = real_cond_timedwait(c, m, t);
    if (!in_hook) note(m);
    return rc;
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
    if (!table) { fprintf(out, "threadcensus: no table\n"); return; }
    struct entry** seen = (struct entry**)calloc(table_size, sizeof *seen);
    unsigned long nseen = 0;
    unsigned long locks_total = 0, locks_single = 0, locks_many_unknown = 0;
    unsigned long mutexes = 0, single = 0;
    for (unsigned long i = 0; i < table_size && seen; ++i) {
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
    fprintf(out, "threadcensus: %lu mutexes, %lu acquisitions (%lu on mutexes the table could not hold; "
                 "table of %lu)\n",
            mutexes, locks_total + over, over, table_size);
    fprintf(out, "single-thread mutexes: %lu of %lu; their acquisitions: %lu of %lu (%.1f%%)\n",
            single, mutexes, locks_single, locks_total + over,
            locks_total + over ? 100.0 * (double)locks_single / (double)(locks_total + over) : 0.0);
    if (locks_many_unknown)
        fprintf(out, "acquisitions on mutexes with more than %d threads (count not kept): %lu\n",
                MAX_OWNERS, locks_many_unknown);
    if (seen) qsort(seen, nseen, sizeof seen[0], by_locks);
    fprintf(out, "\ntop %lu by acquisitions (address, acquisitions, threads, first acquisition's stack):\n",
            nseen < 20 ? nseen : 20);
    fflush(out);
    for (unsigned long i = 0; seen && i < nseen && i < 20; ++i) {
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
