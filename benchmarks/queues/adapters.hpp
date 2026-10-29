#pragma once

// One shape for every queue under test:
//
//   void push(const T&);
//   bool try_pop(T& out);   // false when empty, never blocks
//
// Variants that store shared_ptr<T> (the shim, its original, the typical
// developer's queue, and the library queues instantiated the same way) copy
// *p into out on pop, so every variant pays one copy of T on the consumer
// side and the comparison isolates the queue. Variants that store T inline
// are the ceiling: they show what the shared_ptr in the API costs.
//
// Which variant a name maps to is in variant_names() at the bottom and is
// the list the workloads dispatch over.

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "queue.hpp"          // shim, via -I shim
#include "generic_original.hpp"
#include "mpsc_list_original.hpp"
#include "spsc_ring.hpp"      // runtime, via -I runtime

#include "readerwriterqueue.h"
#include "concurrentqueue.h"
#include "SPSCQueue.h"
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/lockfree/queue.hpp>

// The queues of the LPRQ paper's evaluation (benchmarks/third_party/lprq):
// the fetch-and-add ring queues the lock-free queue literature measures
// itself against.
#include "FAAArrayQueue.hpp"
#include "LCRQueue.hpp"
#include "LPRQueue.hpp"
#include "LSCQueue.hpp"

// oneTBB is the library docs/vision.md names by name, so it is worth having
// when it is installed; it is a system package rather than a vendored
// header, so the tree has to build without it too.
#if defined(WTC_HAVE_TBB)
#include <tbb/concurrent_queue.h>
#endif

namespace wtc_bench {

// B0: the shim as shipped, tag generic: the segmented queue
// (runtime/segment_queue.hpp).
template <class T> struct shim_generic {
    wtc::queue<T> q;
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { auto p = q.try_pop(); if (!p) return false; out = *p; return true; }
};

// hand: the shim with the tag a human (or the compiler) set to spsc.
template <class T> struct shim_spsc {
    wtc::queue<T> q{wtc::topology::spsc};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { auto p = q.try_pop(); if (!p) return false; out = *p; return true; }
};

// hand-inline: the shim with the tag set to spsc_inline, elements held in
// the ring and each pop borrowed in place. The copy into out is the
// consumer's own use of the element, as in every other variant.
template <class T> struct shim_spsc_inline {
    wtc::queue<T> q{wtc::topology::spsc_inline};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { auto p = q.try_pop(); if (!p) return false; out = *p; return true; }
};

// The same three, popped by transfer: try_pop(T&) moves the element into
// out, so the consumer allocates nothing either. What a program that owns
// its elements gets, on the general-purpose queue, the ring and the
// lending ring (where a transfer copies the element out of its slot).
template <class T> struct shim_generic_move {
    wtc::queue<T> q;
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};
template <class T> struct shim_spsc_move {
    wtc::queue<T> q{wtc::topology::spsc};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};
template <class T> struct shim_inline_move {
    wtc::queue<T> q{wtc::topology::spsc_inline};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};

// The shim with the tag the compiler sets for many producers and one
// consumer: the segmented queue with its consumer's side unshared
// (runtime/segment_queue.hpp).
template <class T> struct shim_mpsc {
    wtc::queue<T> q{wtc::topology::mpsc};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { auto p = q.try_pop(); if (!p) return false; out = *p; return true; }
};

// And for one producer and many consumers: the segments whose cells the
// consumers claim with a compare-and-swap (runtime/spmc_queue.hpp).
template <class T> struct shim_spmc {
    wtc::queue<T> q{wtc::topology::spmc};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { auto p = q.try_pop(); if (!p) return false; out = *p; return true; }
};

// B0-orig: the original file, before the tag existed.
// The shim's list for many producers as it was on 21 Oct 2026, the
// implementation the eval-w9 leg of that day measured as shim_mpsc.
template <class T> struct orig_mpsc {
    wtc::rt::mpsc_list<std::shared_ptr<T>> q;
    void push(const T& v) { q.push(std::make_shared<T>(v)); }
    bool try_pop(T& out) { std::shared_ptr<T> p; if (!q.try_pop(p)) return false; out = *p; return true; }
};

template <class T> struct orig_generic {
    wtc_orig::queue<T> q;
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { auto p = q.try_pop(); if (!p) return false; out = *p; return true; }
};

// B1: what a developer writes first.
template <class T> struct std_mutex_queue {
    std::mutex m;
    std::queue<std::shared_ptr<T>> q;
    void push(const T& v) {
        auto p = std::make_shared<T>(v);
        std::lock_guard<std::mutex> g(m);
        q.push(std::move(p));
    }
    bool try_pop(T& out) {
        std::shared_ptr<T> p;
        {
            std::lock_guard<std::mutex> g(m);
            if (q.empty()) return false;
            p = std::move(q.front());
            q.pop();
        }
        out = *p;
        return true;
    }
};

// B2: moodycamel ReaderWriterQueue, same slot type as the shim ...
template <class T> struct moodycamel_rw_sp {
    moodycamel::ReaderWriterQueue<std::shared_ptr<T>> q{1024};
    void push(const T& v) { q.enqueue(std::make_shared<T>(v)); }
    bool try_pop(T& out) { std::shared_ptr<T> p; if (!q.try_dequeue(p)) return false; out = *p; return true; }
};
// ... and inline, the unbounded ceiling.
template <class T> struct moodycamel_rw_inline {
    moodycamel::ReaderWriterQueue<T> q{1024};
    void push(const T& v) { q.enqueue(v); }
    bool try_pop(T& out) { return q.try_dequeue(out); }
};

// B3: moodycamel ConcurrentQueue, the general-purpose lock-free library.
template <class T> struct moodycamel_cq_sp {
    moodycamel::ConcurrentQueue<std::shared_ptr<T>> q;
    void push(const T& v) { q.enqueue(std::make_shared<T>(v)); }
    bool try_pop(T& out) { std::shared_ptr<T> p; if (!q.try_dequeue(p)) return false; out = *p; return true; }
};

// B4: Rigtorp SPSCQueue, bounded; push spins when full, which the workloads'
// in-flight throttle keeps from happening.
template <class T> struct rigtorp_sp {
    rigtorp::SPSCQueue<std::shared_ptr<T>> q{65536};
    void push(const T& v) { q.push(std::make_shared<T>(v)); }
    bool try_pop(T& out) { auto* p = q.front(); if (!p) return false; out = **p; q.pop(); return true; }
};
template <class T> struct rigtorp_inline {
    rigtorp::SPSCQueue<T> q{65536};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { auto* p = q.front(); if (!p) return false; out = *p; q.pop(); return true; }
};

// B5: boost::lockfree::spsc_queue, bounded ring.
template <class T> struct boost_spsc_sp {
    boost::lockfree::spsc_queue<std::shared_ptr<T>, boost::lockfree::capacity<65536>> q;
    void push(const T& v) { auto p = std::make_shared<T>(v); while (!q.push(p)) {} }
    bool try_pop(T& out) { std::shared_ptr<T> p; if (!q.pop(p)) return false; out = *p; return true; }
};
// B5': boost::lockfree::queue, the classic Michael-Scott MPMC. Its element
// must be trivially copyable, so it carries an owning raw pointer.
template <class T> struct boost_ms_queue {
    boost::lockfree::queue<T*> q{65536};
    void push(const T& v) { T* p = new T(v); while (!q.push(p)) {} }
    bool try_pop(T& out) { T* p; if (!q.pop(p)) return false; out = *p; delete p; return true; }
    ~boost_ms_queue() { T* p; while (q.pop(p)) delete p; }
};

#if defined(WTC_HAVE_TBB)
// B6: oneTBB's unbounded multi-producer multi-consumer queue. Its interface
// is already push/try_pop, so nothing is adapted away.
template <class T> struct onetbb_cq_sp {
    tbb::concurrent_queue<std::shared_ptr<T>> q;
    void push(const T& v) { q.push(std::make_shared<T>(v)); }
    bool try_pop(T& out) { std::shared_ptr<T> p; if (!q.try_pop(p)) return false; out = *p; return true; }
};
#endif

// B7: the ring queues of the lock-free queue literature, from the LPRQ
// paper's artifact: LCRQ (CAS2), LPRQ (its portable form), LSCQ (the
// scalable circular queue, linked) and the fetch-and-add array queue. Each
// takes a thread index for its hazard pointers, handed out here per thread
// of the process, and carries an owning raw pointer, as boost_ms_queue does.
inline int lprq_tid() {
    static std::atomic<int> next{0};
    thread_local const int id = next.fetch_add(1);
    return id;
}
template <class T, class Q> struct lprq_adapter {
    Q q;
    void push(const T& v) { q.enqueue(new T(v), lprq_tid()); }
    bool try_pop(T& out) { T* p = q.dequeue(lprq_tid()); if (!p) return false; out = *p; delete p; return true; }
};
template <class T> using lcrq = lprq_adapter<T, LCRQueue<T>>;
template <class T> using lprq = lprq_adapter<T, LPRQueue<T>>;
template <class T> using lscq = lprq_adapter<T, LSCQueue<T>>;
template <class T> using faaarray = lprq_adapter<T, FAAArrayQueue<T>>;

// ceiling: our own ring with T inline and blocks of 65536 slots, which is
// the footprint of the flat 64K-entry ring a person writes for exactly this
// case (rigtorp_inline is that ring). Not what the compiler emits.
template <class T> struct ring_inline {
    wtc::rt::spsc_ring<T, 65536> q;
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};
// and our ring with shared_ptr slots but without the shim's tag branch, to
// price the branch itself.
template <class T> struct ring_sp {
    wtc::rt::spsc_ring<std::shared_ptr<T>> q;
    void push(const T& v) { q.push(std::make_shared<T>(v)); }
    bool try_pop(T& out) { std::shared_ptr<T> p; if (!q.try_pop(p)) return false; out = *p; return true; }
};

// and our ring holding T inline, popped by borrowing in place, at the
// shim's block size: shim_spsc_inline without the tag branch and the
// owner-less shared_ptr, to price those two.
template <class T> struct ring_borrowed {
    wtc::rt::spsc_ring<T> q;
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { T* p = q.try_pop_borrow(); if (!p) return false; out = *p; return true; }
};

// W13, many producers and one consumer with and without an order between
// producers. Every one of these holds T in its cell and is popped by moving
// it out, so nothing is allocated per element and what differs is the
// queue: the shim's single-consumer segmented queue (linearisable), its
// laned queue (each producer's order only; in a build that defines
// WTC_LANED_MPSC), moodycamel's ConcurrentQueue by value (each producer's
// order only, producers found by thread id, no token), and a deque under a
// mutex by value. The shim's general queue popped by moving,
// shim_generic_move, is above.
template <class T> struct shim_mpsc_move {
    wtc::queue<T> q{wtc::topology::mpsc};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};
#ifdef WTC_LANED_MPSC
template <class T> struct shim_laned_move {
    wtc::queue<T> q{wtc::topology::mpsc_laned};
    void push(const T& v) { q.push(v); }
    bool try_pop(T& out) { return q.try_pop(out); }
};
#endif
template <class T> struct moodycamel_cq_inline {
    moodycamel::ConcurrentQueue<T> q;
    void push(const T& v) { q.enqueue(v); }
    bool try_pop(T& out) { return q.try_dequeue(out); }
};
template <class T> struct std_mutex_inline {
    std::mutex m;
    std::deque<T> q;
    void push(const T& v) {
        std::lock_guard<std::mutex> g(m);
        q.push_back(v);
    }
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> g(m);
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        return true;
    }
};

inline const std::vector<std::string>& variant_names() {
    static const std::vector<std::string> v = {
        "shim_generic", "shim_spsc", "shim_spsc_inline", "shim_mpsc", "shim_spmc", "orig_generic", "orig_mpsc", "std_mutex",
        "shim_generic_move", "shim_spsc_move", "shim_inline_move",
        "moodycamel_rw_sp", "moodycamel_rw_inline", "moodycamel_cq_sp",
        "rigtorp_sp", "rigtorp_inline", "boost_spsc_sp", "boost_ms_queue",
        "ring_inline", "ring_sp", "ring_borrowed",
        "lcrq", "lprq", "lscq", "faaarray",
#if defined(WTC_HAVE_TBB)
        "onetbb_cq_sp",
#endif
    };
    return v;
}

// Dispatch by name: calls f.template operator()<Adapter<T>>().
template <class T, class F>
bool with_variant(const std::string& name, F&& f) {
    if (name == "shim_generic")          { f.template run<shim_generic<T>>(); return true; }
    if (name == "shim_spsc")             { f.template run<shim_spsc<T>>(); return true; }
    if (name == "shim_spsc_inline")      { f.template run<shim_spsc_inline<T>>(); return true; }
    if (name == "shim_mpsc")             { f.template run<shim_mpsc<T>>(); return true; }
    if (name == "shim_spmc")             { f.template run<shim_spmc<T>>(); return true; }
    if (name == "orig_generic")          { f.template run<orig_generic<T>>(); return true; }
    if (name == "orig_mpsc")             { f.template run<orig_mpsc<T>>(); return true; }
    if (name == "std_mutex")             { f.template run<std_mutex_queue<T>>(); return true; }
    if (name == "moodycamel_rw_sp")      { f.template run<moodycamel_rw_sp<T>>(); return true; }
    if (name == "moodycamel_rw_inline")  { f.template run<moodycamel_rw_inline<T>>(); return true; }
    if (name == "moodycamel_cq_sp")      { f.template run<moodycamel_cq_sp<T>>(); return true; }
    if (name == "rigtorp_sp")            { f.template run<rigtorp_sp<T>>(); return true; }
    if (name == "rigtorp_inline")        { f.template run<rigtorp_inline<T>>(); return true; }
    if (name == "boost_spsc_sp")         { f.template run<boost_spsc_sp<T>>(); return true; }
    if (name == "boost_ms_queue")        { f.template run<boost_ms_queue<T>>(); return true; }
    if (name == "ring_inline")           { f.template run<ring_inline<T>>(); return true; }
    if (name == "ring_sp")               { f.template run<ring_sp<T>>(); return true; }
    if (name == "ring_borrowed")         { f.template run<ring_borrowed<T>>(); return true; }
    if (name == "shim_generic_move")     { f.template run<shim_generic_move<T>>(); return true; }
    if (name == "shim_spsc_move")        { f.template run<shim_spsc_move<T>>(); return true; }
    if (name == "shim_inline_move")      { f.template run<shim_inline_move<T>>(); return true; }
    if (name == "lcrq")                  { f.template run<lcrq<T>>(); return true; }
    if (name == "lprq")                  { f.template run<lprq<T>>(); return true; }
    if (name == "lscq")                  { f.template run<lscq<T>>(); return true; }
    if (name == "faaarray")              { f.template run<faaarray<T>>(); return true; }
#if defined(WTC_HAVE_TBB)
    if (name == "onetbb_cq_sp")          { f.template run<onetbb_cq_sp<T>>(); return true; }
#endif
    return false;
}

}
