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
 * the share stays honest. A destroyed mutex's entry is retired: its counts
 * go into the table's totals (and its place among the twenty busiest, if
 * it earned one) and its slot is freed, so another mutex at the same
 * address later starts an entry of its own, and a program that creates
 * and destroys millions of objects does not fill the table. A mutex
 * initialised again without being destroyed is retired the same way; a
 * std::mutex is neither initialised nor destroyed through these calls, so
 * one that reuses a dead one's address merges into its entry. A process
 * forked from the first one, or a program it executes with the library
 * still preloaded, writes its report to WTC_CENSUS_OUT.<pid>. The
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

#define TOP 20            /* busiest objects listed in the report */
#define TOMB ((uintptr_t)1) /* a retired entry's key: free for reuse, not the end of a probe */

struct entry {
    _Atomic(uintptr_t) key;            /* the mutex's address, 0 never used, TOMB retired */
    _Atomic(unsigned long) locks;      /* acquisitions */
    _Atomic(unsigned) nowners;         /* distinct threads, MAX_OWNERS+1 means more */
    _Atomic(uintptr_t) owners[MAX_OWNERS];
    void* stack[STACK_DEPTH];          /* the first acquisition's call stack */
    int depth;
    _Atomic(int) stack_state;          /* 0 empty, 1 being written, 2 ready */
};

/* A retired entry kept for the report's list of the busiest. */
struct kept {
    uintptr_t key;
    unsigned long locks;
    unsigned nowners;
    int depth;                         /* 0 when no stack was recorded */
    void* stack[STACK_DEPTH];
};

/* One open-addressing table per kind of object counted: mutexes, and the
 * control blocks of std::shared_ptr. Objects that were destroyed have left
 * the table; their counts are in the retired totals. */
struct table {
    struct entry* e;
    _Atomic(unsigned long) overflow;   /* events on objects the table could not hold */
    _Atomic(unsigned long) ret_count, ret_events, ret_single, ret_single_events, ret_many_events;
    _Atomic(int) ins_lock;             /* serialises inserting and retiring keys; lookups take none */
    _Atomic(int) top_lock;             /* guards top[]; both are spin flags, since the census
                                          cannot take a mutex of its own */
    _Atomic(unsigned long) top_min;    /* the fewest events among kept, once TOP are kept */
    _Atomic(unsigned) nkept;
    struct kept top[TOP];
};
static unsigned table_bits = TABLE_BITS_DEFAULT;
static unsigned long table_size;

/* What happened to control blocks, by kind (wtc_rc_note's operation). */
enum { RC_COPY, RC_RELEASE, RC_LOCK, RC_WEAK_COPY, RC_WEAK_RELEASE, RC_DESTROY, RC_MAKE, RC_KINDS };
static _Atomic(unsigned long) rc_by_kind[RC_KINDS];

static int (*real_lock)(pthread_mutex_t*);
static int (*real_trylock)(pthread_mutex_t*);
static int (*real_unlock)(pthread_mutex_t*);
static int (*real_init)(pthread_mutex_t*, const pthread_mutexattr_t*);
static int (*real_destroy)(pthread_mutex_t*);
static int (*real_cond_wait)(pthread_cond_t*, pthread_mutex_t*);
static int (*real_cond_timedwait)(pthread_cond_t*, pthread_mutex_t*, const struct timespec*);
static _Atomic(int) ready;     /* every symbol resolved and the table mapped */
static __thread int in_hook;   /* backtrace may lock; no counting under it */

static struct table mutexes, blocks;

/* A child of fork has one thread; a spin flag some other thread of the
 * parent held at the fork would never be released in it. */
static void after_fork(void) {
    struct table* ts[2] = {&mutexes, &blocks};
    for (int i = 0; i < 2; ++i) {
        atomic_store(&ts[i]->ins_lock, 0);
        atomic_store(&ts[i]->top_lock, 0);
    }
}

static void spin_lock(_Atomic(int)* f) {
    int free_ = 0;
    while (!atomic_compare_exchange_weak_explicit(f, &free_, 1, memory_order_acquire, memory_order_relaxed)) {
        free_ = 0;
        sched_yield();
    }
}
static void spin_unlock(_Atomic(int)* f) { atomic_store_explicit(f, 0, memory_order_release); }

/* Resolves every symbol and maps the table, once, before any thread but
 * the first can run (the constructor runs at load); a call that arrives
 * earlier still, from the loader, resolves on its own and counts nothing
 * until the table is there. */
static void setup(void) {
    if (atomic_load_explicit(&ready, memory_order_acquire)) return;
    real_lock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_trylock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_unlock = (int (*)(pthread_mutex_t*))dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_init = (int (*)(pthread_mutex_t*, const pthread_mutexattr_t*))dlsym(RTLD_NEXT, "pthread_mutex_init");
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
    if (p != MAP_FAILED) mutexes.e = (struct entry*)p;
    p = mmap(NULL, table_size * sizeof(struct entry), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) blocks.e = (struct entry*)p;
    /* The process that loaded the library first; a child it forks, or a
     * program it executes with the library still preloaded, reports to a
     * file of its own (see report). */
    if (!getenv("WTC_CENSUS_PID")) {
        char pid[24];
        snprintf(pid, sizeof pid, "%ld", (long)getpid());
        setenv("WTC_CENSUS_PID", pid, 0);
    }
    pthread_atfork(NULL, NULL, after_fork);
    atomic_store_explicit(&ready, 1, memory_order_release);
}
__attribute__((constructor)) static void init(void) { setup(); }

static unsigned long home(uintptr_t key) {
    return (unsigned long)(((key >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - table_bits));
}

/* The live entry for key, if there is one: a probe from its home slot past
 * other keys and retired slots (TOMB) to the key or to a never-used slot.
 * Takes no lock: keys change only under ins_lock, and an object's entry is
 * not retired while the object is in use. */
static struct entry* lookup(struct table* t, uintptr_t key) {
    unsigned long h = home(key);
    for (unsigned long probe = 0; probe < table_size; ++probe) {
        struct entry* e = &t->e[(h + probe) & (table_size - 1)];
        uintptr_t k = atomic_load_explicit(&e->key, memory_order_acquire);
        if (k == key) return e;
        if (k == 0) return NULL;
    }
    return NULL;
}

/* The entry for key; with insert, a new one when there is none, in the
 * first retired or never-used slot of its probe. Inserting and retiring
 * are serialised, so a key is never in two slots and a retired slot is
 * reused even when no never-used slot is left on the probe. */
static struct entry* find(struct table* t, uintptr_t key, int insert) {
    if (!t->e || key <= TOMB) return NULL;
    struct entry* e = lookup(t, key);
    if (e || !insert) return e;
    spin_lock(&t->ins_lock);
    e = lookup(t, key);
    if (!e) {
        unsigned long h = home(key);
        for (unsigned long probe = 0; probe < table_size; ++probe) {
            struct entry* f = &t->e[(h + probe) & (table_size - 1)];
            uintptr_t k = atomic_load_explicit(&f->key, memory_order_relaxed);
            if (k == 0 || k == TOMB) {
                atomic_store_explicit(&f->key, key, memory_order_release);
                e = f;
                break;
            }
        }
    }
    spin_unlock(&t->ins_lock);
    return e;
}

/* Keeps a retired entry among the busiest if it is one of them. */
static void keep(struct table* t, struct entry* e, unsigned long l, unsigned n) {
    if (atomic_load_explicit(&t->nkept, memory_order_relaxed) == TOP &&
        l <= atomic_load_explicit(&t->top_min, memory_order_relaxed))
        return;
    spin_lock(&t->top_lock);
    unsigned at = atomic_load_explicit(&t->nkept, memory_order_relaxed);
    if (at == TOP) {   /* replace the least busy */
        at = 0;
        for (unsigned i = 1; i < TOP; ++i)
            if (t->top[i].locks < t->top[at].locks) at = i;
        if (t->top[at].locks >= l) at = TOP;
    } else {
        atomic_store_explicit(&t->nkept, at + 1, memory_order_relaxed);
    }
    if (at < TOP) {
        struct kept* k = &t->top[at];
        k->key = atomic_load(&e->key); k->locks = l; k->nowners = n;
        k->depth = atomic_load_explicit(&e->stack_state, memory_order_acquire) == 2 ? e->depth : 0;
        memcpy(k->stack, e->stack, sizeof k->stack);
    }
    if (atomic_load_explicit(&t->nkept, memory_order_relaxed) == TOP) {
        unsigned long m = t->top[0].locks;
        for (unsigned i = 1; i < TOP; ++i)
            if (t->top[i].locks < m) m = t->top[i].locks;
        atomic_store_explicit(&t->top_min, m, memory_order_relaxed);
    }
    spin_unlock(&t->top_lock);
}

/* An object is gone, or a new one starts at its address: its counts go
 * into the table's totals and its slot is freed. A retired slot followed
 * by a never-used one becomes never-used itself, and so do the retired
 * slots before it, so probes stay as short as the live keys make them.
 * Nothing else touches an object while it is destroyed or initialised. */
static void retire(struct table* t, uintptr_t key) {
    if (!t->e || key <= TOMB) return;
    spin_lock(&t->ins_lock);
    struct entry* e = lookup(t, key);
    if (e) {
        unsigned long l = atomic_load(&e->locks);
        unsigned n = atomic_load(&e->nowners);
        if (l) {
            atomic_fetch_add(&t->ret_count, 1);
            atomic_fetch_add(&t->ret_events, l);
            if (n == 1) { atomic_fetch_add(&t->ret_single, 1); atomic_fetch_add(&t->ret_single_events, l); }
            if (n > MAX_OWNERS) atomic_fetch_add(&t->ret_many_events, l);
            keep(t, e, l, n);
        }
        atomic_store(&e->locks, 0);
        for (unsigned i = 0; i < MAX_OWNERS; ++i) atomic_store(&e->owners[i], 0);
        atomic_store(&e->nowners, 0);
        e->depth = 0;
        atomic_store(&e->stack_state, 0);
        unsigned long i = (unsigned long)(e - t->e);
        uintptr_t mark = atomic_load(&t->e[(i + 1) & (table_size - 1)].key) == 0 ? 0 : TOMB;
        atomic_store_explicit(&e->key, mark, memory_order_release);
        while (mark == 0) {
            i = (i - 1) & (table_size - 1);
            if (atomic_load(&t->e[i].key) != TOMB) break;
            atomic_store_explicit(&t->e[i].key, 0, memory_order_release);
        }
    }
    spin_unlock(&t->ins_lock);
}

/* One event on an object: counted, its thread remembered, and the stack of
 * the first event kept. */
static void note_in(struct table* t, uintptr_t key) {
    struct entry* e = find(t, key, 1);
    if (!e) { atomic_fetch_add_explicit(&t->overflow, 1, memory_order_relaxed); return; }
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

static void note(pthread_mutex_t* m) { note_in(&mutexes, (uintptr_t)m); }

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

/* A mutex initialised where another lived (one freed without being
 * destroyed, which C programs do) starts an entry of its own: the old one
 * is retired here as it would have been by pthread_mutex_destroy. A
 * std::mutex calls neither: libstdc++ initialises it statically and never
 * destroys it, so one that reuses a dead one's address merges into its
 * entry. */
int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* a) {
    setup();
    retire(&mutexes, (uintptr_t)m);
    return real_init(m, a);
}

/* A destroyed mutex's entry is retired: its counts stay in the report,
 * and a mutex that later lives at the same address starts an entry of its
 * own rather than merging into it. */
int pthread_mutex_destroy(pthread_mutex_t* m) {
    setup();
    retire(&mutexes, (uintptr_t)m);
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

/* Reference counts. libstdc++'s are inline atomics, so nothing here can
 * interpose them; the program calls in instead, in one of two ways.
 *
 * wtc_rc_note is what `wtc-cc --census` plants at the entry of each of
 * _Sp_counted_base's count functions (the --wtc-census pass): the control
 * block's making (its constructor, which sets both counts to one, so the
 * thread that made a block counts as one that touched it), every copy,
 * release, weak_ptr lock, weak copy and weak release of a shared_ptr, and
 * the block's destruction, which retires its entry as
 * pthread_mutex_destroy retires a mutex's. A block made where another
 * lived retires that one's entry first, in case its destruction was not
 * seen.
 *
 * wtc_rc_before and wtc_rc_after serve a program built by an ordinary
 * compiler with tools/census/rc_hooks.h included first: that header
 * defines libstdc++'s race-detector macros, which _Sp_counted_base invokes
 * on every release (before, on the use count) and before destroying the
 * block (after, on the weak count). That sees releases and destruction but
 * not copies; every copy is released once, so the operation count is about
 * half, a thread that only ever copies a pointer it never releases is
 * missed, and the block's making is not seen, all of which err towards
 * "one thread". */
void wtc_rc_note(void* ctrl, int op) {
    setup();
    if (in_hook || op < 0 || op >= RC_KINDS) return;
    atomic_fetch_add_explicit(&rc_by_kind[op], 1, memory_order_relaxed);
    if (op == RC_DESTROY || op == RC_MAKE) retire(&blocks, (uintptr_t)ctrl);
    if (op != RC_DESTROY) note_in(&blocks, (uintptr_t)ctrl);
}

/* The use count follows the vtable pointer, the weak count follows it. */
void wtc_rc_before(const void* ctrl, long offset) {
    if (offset == (long)sizeof(void*)) wtc_rc_note((void*)ctrl, RC_RELEASE);
}
void wtc_rc_after(const void* ctrl, long offset) {
    if (offset == (long)(sizeof(void*) + sizeof(int))) wtc_rc_note((void*)ctrl, RC_DESTROY);
}

static int by_locks(const void* a, const void* b) {
    unsigned long x = ((const struct kept*)a)->locks;
    unsigned long y = ((const struct kept*)b)->locks;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* One section of the report: how many objects, how many events, what share
 * fell on objects one thread touched, and the twenty busiest. */
static void report_table(FILE* out, struct table* t, const char* objects, const char* events) {
    /* The objects still alive, and the busiest of those destroyed. */
    struct kept* seen = (struct kept*)calloc(table_size + TOP, sizeof *seen);
    unsigned long nseen = 0;
    unsigned long ev_total = atomic_load(&t->ret_events), ev_single = atomic_load(&t->ret_single_events);
    unsigned long ev_many_unknown = atomic_load(&t->ret_many_events);
    unsigned long count = atomic_load(&t->ret_count), single = atomic_load(&t->ret_single);
    for (unsigned i = 0; seen && i < t->nkept; ++i) seen[nseen++] = t->top[i];
    for (unsigned long i = 0; i < table_size; ++i) {
        struct entry* e = &t->e[i];
        if (atomic_load(&e->key) <= TOMB) continue;
        unsigned long l = atomic_load(&e->locks);
        if (l == 0) continue;
        unsigned n = atomic_load(&e->nowners);
        ++count; ev_total += l;
        if (n == 1) { ++single; ev_single += l; }
        if (n > MAX_OWNERS) ev_many_unknown += l;
        if (!seen) continue;
        struct kept* k = &seen[nseen++];
        k->key = atomic_load(&e->key); k->locks = l; k->nowners = n;
        k->depth = atomic_load(&e->stack_state) == 2 ? e->depth : 0;
        memcpy(k->stack, e->stack, sizeof k->stack);
    }
    unsigned long over = atomic_load(&t->overflow);
    fprintf(out, "threadcensus: %lu %s, %lu %s (%lu on %s the table could not hold; table of %lu)\n",
            count, objects, ev_total + over, events, over, objects, table_size);
    fprintf(out, "single-thread %s: %lu of %lu; their %s: %lu of %lu (%.1f%%)\n",
            objects, single, count, events, ev_single, ev_total + over,
            ev_total + over ? 100.0 * (double)ev_single / (double)(ev_total + over) : 0.0);
    if (ev_many_unknown)
        fprintf(out, "%s on %s with more than %d threads (count not kept): %lu\n",
                events, objects, MAX_OWNERS, ev_many_unknown);
    if (seen) qsort(seen, nseen, sizeof seen[0], by_locks);
    fprintf(out, "\ntop %lu by %s (address, %s, threads, first event's stack):\n",
            nseen < 20 ? nseen : 20, events, events);
    fflush(out);
    for (unsigned long i = 0; seen && i < nseen && i < 20; ++i) {
        struct kept* k = &seen[i];
        unsigned n = k->nowners;
        fprintf(out, "\n%#lx  %lu  %s%u\n", (unsigned long)k->key, k->locks,
                n > MAX_OWNERS ? ">" : "", n > MAX_OWNERS ? MAX_OWNERS : n);
        fflush(out);
        if (k->depth > 2) backtrace_symbols_fd(k->stack + 2, k->depth - 2, fileno(out));
    }
    free(seen);
}

__attribute__((destructor)) static void report(void) {
    const char* path = getenv("WTC_CENSUS_OUT");
    /* A process forked from the first one, or a program it executed with
     * the library still preloaded, writes beside it, to <path>.<pid>. */
    char forked[4096];
    const char* first = getenv("WTC_CENSUS_PID");
    if (path && first && atol(first) != (long)getpid()) {
        snprintf(forked, sizeof forked, "%s.%ld", path, (long)getpid());
        path = forked;
    }
    FILE* out = path ? fopen(path, "w") : stderr;
    if (!out) out = stderr;
    if (!mutexes.e || !blocks.e) { fprintf(out, "threadcensus: no table\n"); return; }
    report_table(out, &mutexes, "mutexes", "acquisitions");
    unsigned long rc = 0;
    for (int k = 0; k < RC_KINDS; ++k) rc += atomic_load(&rc_by_kind[k]);
    if (rc) {
        fprintf(out, "\nreference counts: %lu control blocks made, %lu copies, %lu releases, %lu weak_ptr "
                     "locks, %lu weak copies, %lu weak releases, %lu control blocks destroyed\n",
                atomic_load(&rc_by_kind[RC_MAKE]), atomic_load(&rc_by_kind[RC_COPY]), atomic_load(&rc_by_kind[RC_RELEASE]),
                atomic_load(&rc_by_kind[RC_LOCK]), atomic_load(&rc_by_kind[RC_WEAK_COPY]),
                atomic_load(&rc_by_kind[RC_WEAK_RELEASE]), atomic_load(&rc_by_kind[RC_DESTROY]));
        report_table(out, &blocks, "control blocks", "count operations");
    }
    if (out != stderr) fclose(out);
}
