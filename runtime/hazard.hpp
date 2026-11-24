// wtc::rt::hazard: hazard pointers for the runtime's queues that free memory
// another thread may still be reading ("Hazard Pointers: Safe Memory
// Reclamation for Lock-Free Objects", IEEE TPDS 2004).
//
// A thread that is about to read a shared node publishes the node's address
// in one of its slots and checks that the node is still reachable; a thread
// that has made a node unreachable retires it, and the node is freed only
// once no slot holds it. The cost to a reader is one sequentially consistent
// store and a reload per protected pointer.
//
// One domain serves the whole process. Each thread that touches a queue
// holds one record while it lives. A thread that exits leaves the nodes it
// retired and could not yet free in its record, and whichever thread takes
// the record next frees them in its scans.
//
// Where the records come from, and what a thread's first operation costs.
// The records live in chunks of kChunk, the first made when the first queue
// is built (prepare()), and are reused, never freed, so that a scan may walk
// them while threads come and go. A thread finds its record through a
// thread_local pointer that is constant-initialised and trivially
// destructible, so reading it registers nothing: a thread_local with a
// destructor registers it at the thread's first use, through
// __cxa_thread_atexit_impl, which allocates and takes the dynamic linker's
// load lock (a dlopen in another thread holds it for as long as it runs).
// The record is given back by the destructor of a pthread key created once,
// with the first chunk. A thread's first operation takes an idle record
// with one compare-and-swap, or the next unused one with a fetch-and-add,
// and sets the key: lock-free, and without the allocator unless more than
// kChunk threads hold records at once, when a chunk is added, or unless the
// key is not among the first 32 the process created.
//
// That last condition is the C library's: glibc keeps a thread's values of
// the first 32 keys in the thread's own descriptor, and the values of later
// keys in blocks it allocates (calloc) at the thread's first
// pthread_setspecific of a key in the block. key_allocates() says which
// case this process is in (it is true where the C library is not glibc,
// which is not known not to allocate). The key is created by the first
// queue's constructor, so a program that creates 32 keys before it builds
// any queue pays one calloc in each thread's first operation. If that
// calloc fails, pthread_setspecific fails (ENOMEM) and the thread's exit
// cannot give its record back; the thread then keeps its record untied
// (unhooked() counts these failures) and tries to set the key again at its
// next operation. A thread that exits untied leaves its record taken for
// good, with the nodes it retired and could not free: memory, not
// correctness.
//
// The key's destructor runs after the thread's C++ thread_local
// destructors (glibc runs those first), so a queue operation from one of
// them still finds the thread's record. One from a key destructor that
// runs after it (glibc runs the key destructors in up to four rounds,
// PTHREAD_DESTRUCTOR_ITERATIONS, and a key set again in the last one is
// never destroyed) must not take a record tied to that exit, which might
// never give it back: the thread is exiting (t_exiting, set by release()),
// so the operation borrows a record for its own length and gives it back
// as it returns (use).
// The main thread's record is not given back at exit(), which runs no key
// destructors; its retired nodes stay reachable from the chunk.
//
// After fork() only the forking thread runs in the child, and the records
// the parent's other threads held are given back there (after_fork, a
// pthread_atfork handler registered with the key): their slots are
// cleared and their retired nodes stay for the record's next holder. An
// operation another thread was inside at the fork is abandoned in the
// child, with whatever it held; the child may use a queue only if no
// other thread was inside an operation on it at the fork (POSIX allows
// the child of a multi-threaded process less than that). _Fork() and a raw
// clone run no atfork handler: in a child made so, the records the other
// threads held stay taken.
#pragma once

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

namespace wtc::rt::hazard {

constexpr unsigned kSlots = 2;
// Which slot each queue operation protects its segment with, the same on
// every queue: pushes use kPushSlot, pops kPopSlot. A pop runs the
// program's code while it still needs its segment -- the taker, which moves
// the element out and may destroy the destination's old value -- and that
// code may push to any queue: a push never touches the pop's slot, so it
// cannot unprotect the segment under the pop. A pop inside another pop's
// taker would overwrite the slot and is not allowed (the shim states it as
// a rule of the element type: its move, copy and destructor may push to a
// wtc::queue but not pop from one).
constexpr unsigned kPushSlot = 0;
constexpr unsigned kPopSlot = 1;
// A scan reads every record's slots, so it waits for this many retired
// nodes. The queues retire one segment per thousand-odd elements, so the
// nodes waiting are a few segments at most.
constexpr std::size_t kScanAt = 8;

// What a retired object carries, so that retiring it and scanning never
// allocate: the retired nodes are an intrusive list through these fields,
// which only the thread holding the retiring record touches. An object
// that is retired derives from node and is freed with delete.
struct node {
    node* hp_next = nullptr;
    void* hp_addr = nullptr;  // the address a slot holds while it is protected
    void (*hp_free)(node*) = nullptr;
    bool hp_held = false;     // a scan's mark
};

// Records per chunk: the threads that may hold records at once before a
// first operation allocates.
#ifndef WTC_HAZARD_CHUNK
#define WTC_HAZARD_CHUNK 256
#endif
constexpr unsigned kChunk = WTC_HAZARD_CHUNK;

// One line each: a reader's slot store does not take a neighbour's line.
struct alignas(64) record {
    std::atomic<void*> slot[kSlots];
    std::atomic<bool> active{false};
    // Its holder's only: the nodes it retired and has not freed.
    node* retired = nullptr;
    std::size_t retired_count = 0;

    record() {
        for (auto& s : slot) s.store(nullptr, std::memory_order_relaxed);
    }
};

struct chunk {
    record recs[kChunk];
    // Records handed out at least once: a scan reads these and no more.
    std::atomic<unsigned> used{0};
    std::atomic<chunk*> next{nullptr};
};

inline std::atomic<chunk*> g_chunks{nullptr};
inline pthread_key_t g_key;
inline thread_local record* t_record = nullptr;
// A record this thread holds whose key could not be set (see the top).
inline thread_local record* t_untied = nullptr;
// Set when this thread's exit has given its record back (release()).
inline thread_local bool t_exiting = false;
inline std::atomic<std::uint64_t> g_unhooked{0};

inline void release(void* r);
inline void after_fork();

// The domain's one-time set-up: the first chunk and the key. Called by
// every queue's constructor, so every operation finds it done (the
// construction happens before them); the static's guard is taken on that
// path only.
inline void prepare() {
    static const bool once = [] {
        if (int e = pthread_key_create(&g_key, &release)) {
            std::fprintf(stderr, "wtc: hazard pointers: pthread_key_create failed (%d)\n", e);
            std::abort();
        }
        if (int e = pthread_atfork(nullptr, nullptr, &after_fork)) {
            std::fprintf(stderr, "wtc: hazard pointers: pthread_atfork failed (%d)\n", e);
            std::abort();
        }
        g_chunks.store(new chunk, std::memory_order_seq_cst);
        return true;
    }();
    (void)once;
}

// Whether a thread's first pthread_setspecific of the key allocates (see
// the top): glibc's pthread_key_t is the key's index, and the first 32
// (PTHREAD_KEY_2NDLEVEL_SIZE) live in the thread's descriptor. Valid once
// prepare() has run.
inline bool key_allocates() {
#ifdef __GLIBC__
    return static_cast<unsigned long>(g_key) >= 32;
#else
    return true;
#endif
}

// pthread_setspecific failures, each leaving a thread's record untied for
// the time being; for tests.
inline std::uint64_t unhooked() { return g_unhooked.load(std::memory_order_relaxed); }

// Every record that may hold a slot: the used prefix of each chunk. The
// loads are sequentially consistent, as the fetch-and-add and the link
// that make a record usable are: a reader's slot store follows those, so a
// scan whose unlink follows the slot store sees the record.
template <class F>
void for_each_record(F&& f) {
    for (chunk* c = g_chunks.load(std::memory_order_seq_cst); c; c = c->next.load(std::memory_order_seq_cst)) {
        const unsigned n = std::min(c->used.load(std::memory_order_seq_cst), kChunk);
        for (unsigned i = 0; i < n; ++i) f(c->recs[i]);
    }
}

inline bool take_idle(record& r) {
    bool idle = false;
    return !r.active.load(std::memory_order_relaxed) &&
           r.active.compare_exchange_strong(idle, true, std::memory_order_acq_rel);
}

// An idle record, or a fresh one. A compare-and-swap fails only because
// another thread took that record, and each record is tried once, so the
// walk is bounded by the records there are.
inline record* acquire() {
    chunk* c = g_chunks.load(std::memory_order_seq_cst);
    for (;;) {
        const unsigned n = std::min(c->used.load(std::memory_order_seq_cst), kChunk);
        for (unsigned i = 0; i < n; ++i)
            if (take_idle(c->recs[i])) return &c->recs[i];
        while (c->used.load(std::memory_order_relaxed) < kChunk) {
            const unsigned i = c->used.fetch_add(1, std::memory_order_seq_cst);
            if (i >= kChunk) break;
            if (take_idle(c->recs[i])) return &c->recs[i];
        }
        chunk* next = c->next.load(std::memory_order_seq_cst);
        if (!next) {
            // More than kChunk threads hold records: the one allocation a
            // first operation can make.
            chunk* fresh = new chunk;
            if (c->next.compare_exchange_strong(next, fresh, std::memory_order_seq_cst))
                next = fresh;
            else
                delete fresh;
        }
        c = next;
    }
}

// Frees every node this record retired that no slot in any record holds.
// It allocates nothing: each slot is read once, and a slot that holds a
// pointer marks the retired node it names. Its cost is the slots of the
// records handed out plus, for each slot that holds anything, the nodes
// waiting: at most kScanAt plus those held at the last scan.
inline void scan(record& mine) {
    if (!mine.retired) return;
    for_each_record([&](record& r) {
        for (auto& s : r.slot)
            if (void* p = s.load(std::memory_order_seq_cst))
                for (node* n = mine.retired; n; n = n->hp_next)
                    if (n->hp_addr == p) n->hp_held = true;
    });
    node** link = &mine.retired;
    while (node* n = *link) {
        if (n->hp_held) {
            n->hp_held = false;
            link = &n->hp_next;
        } else {
            *link = n->hp_next;
            --mine.retired_count;
            n->hp_free(n);
        }
    }
}

// The key's destructor, at the thread's exit: clears the slots, frees what
// it can, and gives the record back.
inline void release(void* p) {
    record* r = static_cast<record*>(p);
    for (auto& s : r->slot) s.store(nullptr, std::memory_order_release);
    scan(*r);
    t_record = nullptr;
    t_exiting = true;
    r->active.store(false, std::memory_order_release);
}

// In a fork's child: every record held at the fork by a thread other than
// this one is given back, as that thread's exit would have.
inline void after_fork() {
    for_each_record([](record& r) {
        if (&r == t_record || &r == t_untied || !r.active.load(std::memory_order_relaxed)) return;
        for (auto& s : r.slot) s.store(nullptr, std::memory_order_relaxed);
        r.active.store(false, std::memory_order_release);
    });
}

// The thread's first operation, or the next one after a key that could not
// be set (see the top).
[[gnu::noinline]] inline record& first_use() {
    prepare();  // done already wherever a queue was built first
    record* r = t_untied ? t_untied : acquire();
    if (pthread_setspecific(g_key, r) != 0) {
        t_untied = r;
        g_unhooked.fetch_add(1, std::memory_order_relaxed);
        return *r;
    }
    t_untied = nullptr;
    t_record = r;
    return *r;
}

// The thread's record, taken at its first call. For code outside a queue
// operation (tests); an operation takes its record through use, which
// does not tie one to an exit that has already run.
inline record& mine() {
    if (record* r = t_record) return *r;
    return first_use();
}

// The thread's record for the length of one operation: its own, tied to
// its exit (or untied, see the top), or, once its exit has given that one
// back, a record borrowed for the operation and given back at its end,
// with the nodes the operation retired and could not free left for the
// record's next holder.
//
// Its fast path, a thread_local read and a test, is inline in every
// operation; the rest is out of line. As a call of its own it cost the
// general-purpose queue some 15 percent at one producer and one consumer.
class use {
public:
    use() : r_(t_record) {
        if (__builtin_expect(r_ == nullptr, 0)) r_ = take();
    }
    // A record is borrowed exactly when the thread's exit has run (a thread
    // that has not exited takes or has its own), so the flag is t_exiting
    // itself, and nothing of the object but r_ needs to live in memory.
    ~use() {
        if (__builtin_expect(t_exiting, 0)) give_back(r_);
    }
    use(const use&) = delete;
    use& operator=(const use&) = delete;
    operator record&() const { return *r_; }

private:
    [[gnu::noinline]] static record* take() {
        if (t_exiting) {
            prepare();
            return acquire();
        }
        return &first_use();
    }
    [[gnu::noinline]] static void give_back(record* r) {
        for (auto& s : r->slot) s.store(nullptr, std::memory_order_release);
        scan(*r);
        r->active.store(false, std::memory_order_release);
    }

    record* r_;
};

// Records handed out at least once, over all chunks; for tests.
inline unsigned records_made() {
    unsigned n = 0;
    for_each_record([&](record&) { ++n; });
    return n;
}

// Reads src and keeps what it read from being freed until clear(i): the
// value returned was reachable from src after the slot was published, so a
// thread that unlinks it afterwards will find the slot when it scans.
template <class T>
T* protect(record& r, unsigned i, const std::atomic<T*>& src) {
    T* p = src.load(std::memory_order_relaxed);
    for (;;) {
        r.slot[i].store(p, std::memory_order_seq_cst);
        T* again = src.load(std::memory_order_seq_cst);
        if (again == p) return p;
        p = again;
    }
}

inline void clear(record& r, unsigned i) { r.slot[i].store(nullptr, std::memory_order_release); }

// The node of an object that is retired: the object itself where it
// derives from node, otherwise its member hp, which the object places
// where it costs nothing (the segmented queues keep it in a line their
// hot fields leave unused, so that their layout is the one they had
// before they carried a node).
template <class T>
node& node_of(T* p) {
    if constexpr (std::is_base_of_v<node, T>) return *p;
    else return p->hp;
}

// p is unreachable from every shared pointer a reader could protect it
// through; it is deleted once no slot holds it. Never allocates.
template <class T>
void retire(record& r, T* p) {
    node* n = &node_of(p);
    n->hp_addr = static_cast<void*>(p);
    n->hp_free = [](node* x) { delete static_cast<T*>(x->hp_addr); };
    n->hp_held = false;
    n->hp_next = r.retired;
    r.retired = n;
    if (++r.retired_count >= kScanAt) scan(r);
}

}  // namespace wtc::rt::hazard
