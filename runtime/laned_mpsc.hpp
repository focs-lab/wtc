// wtc::rt::laned_mpsc: an unbounded queue for any number of producers and
// one consumer that keeps each producer's order and no order between
// producers. Every producer thread pushes into a single-producer ring of
// its own, a lane, and the consumer takes from the lanes in turn.
//
// What the compiler must prove before substituting this for wtc::queue:
//
//   Over the object's whole lifetime exactly one thread calls try_pop (or
//   consume), any number call push, and the destructor runs after all of
//   them have made their last call (ordered by a join or equivalent); and
//   no thread can observe the order in which elements of two different
//   producers come out, beyond what the order within each producer already
//   fixes.
//
// The first part is the single-consumer form's fact (segment_queue.hpp).
// The second is new: the queue is not linearisable. A push that finished
// before another producer's push began may come out after it. Per producer
// the order is FIFO, as decisions/0003 requires, but its other half, that
// the queue as a whole behaves as one sequence, is given up. A program
// whose consumer only aggregates (counts, sums, writes each element to a
// sink that sorts or does not care), or whose producers never communicate
// except through the queue, cannot tell; one that pushes on thread A, tells
// thread B, and then pushes on B expecting A's element first, can.
//
// What breaks if the first part is false: a second consumer races on the
// consumer's lane list and position, and on the lanes' consumer sides as
// it would on a ring's (spsc_ring.hpp). With WTC_CHECK it aborts at its
// first pop. If the second part is false, nothing is lost or duplicated:
// elements of different producers come out in an order the program did
// not expect.
//
// Semantics kept: every push happens-before the pop that returns its
// element; FIFO per producer thread, batches included; push never blocks
// on the consumer and never fails; no capacity. try_pop never blocks and
// answers empty only when every push that finished before it began has
// been popped: such a push registered its lane before it pushed, the
// consumer reads the registry's version before it sweeps, and the lane's
// ring answers empty only when it was (spsc_ring.hpp).
//
// Design. A producer finds its lane through a table of its own thread's
// (a thread_local, keyed by a queue number that is never reused, with the
// last lookup cached in front), so a push is the ring's push plus one
// comparison. Its first push to a queue makes the lane and registers it
// under the queue's mutex, the only lock, taken once per producer per
// queue; registering bumps a version the consumer reads once per sweep.
// The consumer keeps a copy of the lane list and stays on one lane for up
// to kBurst elements, then moves to the next, so a run is read from one
// ring while its lines are still in this core's cache, and no lane waits
// longer than one turn of the others: with L lanes, an element at the
// front of its lane is taken within (L - 1) * kBurst pops of others. A pop
// that finds its lane empty sweeps the others once, in turn, and answers
// empty only after a whole sweep found nothing; an empty poll costs one
// read of each lane's published count, a line that stays in the
// consumer's cache until its producer publishes again.
//
// A producer thread that exits marks its lanes exited (its table's
// destructor). The consumer drains such a lane like any other, and when it
// finds it exited and then empty, nothing can arrive in it any more: it
// takes the lane out of the registry and frees it. The mark is in a small
// block of its own, shared by the lane and the producer's table and freed
// by whichever lets go of it last, because either may go first: a thread
// may outlive a queue it pushed to, and a queue may outlive its producers.
// A table drops the entries of destroyed queues when it next registers a
// lane, so it holds one entry per live queue the thread has pushed to.
//
// Costs. A lane is a ring of two blocks at rest (lane_slots below), so
// memory grows with the number of producers that have ever pushed and not
// exited, not with the elements in flight. Every producer thread pays one
// allocation of a lane per queue on its first push. A sweep over an empty
// queue reads every lane: with many producers that each push rarely, a
// lone element waits for the consumer to reach its lane, up to one read of
// every other lane's count.
//
// The ring keeps its consumer's slip (spsc_ring.hpp): when the consumer
// catches up on a lane whose producer streams it waits a bounded while for
// a batch. With one producer this queue is the ring.
//
// Lineage: per-producer sub-queues scanned by one consumer are what
// moodycamel's ConcurrentQueue does for producers without a token (it
// finds the producer's sub-queue by a hash of the thread id), and what the
// "FFQ" and Jiffy MPSC papers argue against for linearisable queues; the
// relaxed-order argument is that of "k-FIFO" and "quasi-linearizable"
// queues, here with per-producer order kept exactly.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "spsc_ring.hpp"

#ifdef WTC_CHECK
#include <pthread.h>
#endif

namespace wtc::rt {

namespace laned_detail {

// Shared by one lane and one producer thread's table, and freed by
// whichever of the two lets go last.
struct lane_mark {
    // Set by the producer's thread as it exits, after its last push:
    // nothing more will arrive in the lane.
    std::atomic<bool> exited{false};
    // Set by the queue's destructor: the lane is gone, and the table's
    // pointer to it must not be used.
    std::atomic<bool> queue_gone{false};
    std::atomic<unsigned> refs{2};
};

inline void let_go(lane_mark* m) {
    if (m->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete m;
}

// One producer thread's lanes, in every laned queue it has pushed to.
struct table {
    struct entry {
        std::uint64_t queue;
        void* lane;
        lane_mark* mark;
    };
    std::uint64_t last_queue = 0;
    void* last_lane = nullptr;
    std::vector<entry> entries;

    void* find(std::uint64_t queue) {
        for (const entry& e : entries)
            if (e.queue == queue) {
                last_queue = queue;
                last_lane = e.lane;
                return e.lane;
            }
        return nullptr;
    }

    // Drops the entries of queues that have been destroyed.
    void prune() {
        auto gone = [](const entry& e) {
            if (!e.mark->queue_gone.load(std::memory_order_acquire)) return false;
            let_go(e.mark);
            return true;
        };
        entries.erase(std::remove_if(entries.begin(), entries.end(), gone), entries.end());
        last_queue = 0;
        last_lane = nullptr;
    }

    void add(std::uint64_t queue, void* lane, lane_mark* mark) {
        prune();
        entries.push_back({queue, lane, mark});
        last_queue = queue;
        last_lane = lane;
    }

    // The thread is exiting: every push it made has been made. The release
    // store orders them before the mark, which the consumer reads with
    // acquire before it decides that an empty lane will stay empty. A queue
    // operation from a thread_local destructor that runs after this one on
    // the same thread would find a destroyed table; as with hazard.hpp, the
    // queues are not meant to be used from one.
    ~table() {
        for (const entry& e : entries) {
            e.mark->exited.store(true, std::memory_order_release);
            let_go(e.mark);
        }
    }
};

inline thread_local table t_lanes;

// Queue numbers are never reused, so an entry left by a destroyed queue can
// never be taken for a new queue's at the same address.
inline std::atomic<std::uint64_t> g_next_queue{1};

}  // namespace laned_detail

// Two blocks of a lane's ring hold this many slots each: 32 KiB, and never
// fewer than 256 slots, below which the ring's block turnover shows
// (spsc_ring.hpp). Smaller than the ring's own 128 KiB because a queue has
// a lane per producer.
template <class Slot>
constexpr unsigned lane_slots() {
    constexpr unsigned n = 32 * 1024 / sizeof(Slot);
    return n < 256 ? 256 : n;
}

template <class Slot, unsigned kBurst = 64, unsigned kLaneSlots = lane_slots<Slot>(), unsigned kLine = 64,
          bool kSlip = true>
class laned_mpsc {
    static_assert(kBurst >= 1, "the consumer takes at least one element from a lane");

    using ring = spsc_ring<Slot, kLaneSlots, kLine, kSlip>;

    struct lane {
        ring r;
        // Read by the consumer only, on a line of its own, away from the
        // ring's producer and consumer lines.
        alignas(kLine) laned_detail::lane_mark* mark;
        bool dead = false;  // exited and found empty after: to be freed
        explicit lane(laned_detail::lane_mark* m) : mark(m) {}
    };

 public:
    laned_mpsc() : id_(laned_detail::g_next_queue.fetch_add(1, std::memory_order_relaxed)) {}

    laned_mpsc(const laned_mpsc&) = delete;
    laned_mpsc& operator=(const laned_mpsc&) = delete;

    // By contract no other thread is in an operation on this queue. The
    // lanes go, with whatever elements they still hold; a producer that is
    // still alive keeps only the mark, now saying the queue is gone.
    ~laned_mpsc() {
        for (lane* l : registry_) {
            laned_detail::lane_mark* m = l->mark;
            delete l;
            m->queue_gone.store(true, std::memory_order_release);
            laned_detail::let_go(m);
        }
    }

    template <class S>
    void push(S&& s) {
        lane& l = mine();
        l.r.push(std::forward<S>(s));
    }

    // A producer's run, as on the ring: push_deferred puts the element in
    // this thread's lane unseen, publish makes the run so far visible at
    // once and in order.
    template <class S>
    void push_deferred(S&& s) {
        mine().r.push_deferred(std::forward<S>(s));
    }
    void publish() { mine().r.publish(); }

    // Pops an element into f, called once with the element as an rvalue
    // still in its slot, or not at all when the queue was empty: the next
    // of the current lane's while its burst lasts, else the first a sweep
    // of the lanes from the next one on finds.
    template <class F>
    bool consume(F&& f) {
        static_assert(std::is_nothrow_invocable_v<F, Slot&&>,
                      "the taker runs with the element still in its slot, which is released after "
                      "it returns; it must not throw");
#ifdef WTC_CHECK
        claim_consumer();
#endif
        if (run_ < kBurst && cur_ < lanes_.size() && lanes_[cur_]->r.consume(f)) {
            ++run_;
            return true;
        }
        return sweep(f);
    }

    bool try_pop(Slot& out) {
        return consume([&](Slot&& s) noexcept { out = std::move(s); });
    }

    // Not synchronised; hints for the consumer's own thread: the lanes it
    // knows of, and how many it has freed.
    std::size_t lanes() const { return lanes_.size(); }
    std::uint64_t retired() const { return retired_; }

 private:
    // This thread's lane, made and registered on its first push.
    lane& mine() {
        laned_detail::table& t = laned_detail::t_lanes;
        if (t.last_queue == id_) return *static_cast<lane*>(t.last_lane);
        if (void* found = t.find(id_)) return *static_cast<lane*>(found);
        return enrol(t);
    }

    [[gnu::noinline]] lane& enrol(laned_detail::table& t) {
        auto* m = new laned_detail::lane_mark;
        lane* l = new lane(m);
        {
            std::lock_guard<std::mutex> g(registry_m_);
            registry_.push_back(l);
            version_.fetch_add(1, std::memory_order_release);
        }
        t.add(id_, l, m);
        return *l;
    }

    // Visits every lane once, from the one after the current, the current
    // last; takes the first element found and makes its lane current with
    // a fresh burst. Every kExitEvery-th sweep, a lane that answers empty
    // and whose producer has exited is asked once more after the mark is
    // read: empty then, it stays empty, and it is freed when the sweep
    // ends.
    template <class F>
    bool sweep(F& f) {
        if (version_.load(std::memory_order_acquire) != seen_version_) refresh();
        const std::size_t n = lanes_.size();
        bool got = false;
        const bool check_exits = (++sweeps_ & (kExitEvery - 1)) == 0;
        for (std::size_t k = 1; k <= n; ++k) {
            const std::size_t i = (cur_ + k) % n;
            lane* l = lanes_[i];
            if (l->r.consume(f)) {
                cur_ = i;
                run_ = 1;
                got = true;
                break;
            }
            if (check_exits && l->mark->exited.load(std::memory_order_acquire)) {
                if (l->r.consume(f)) {
                    cur_ = i;
                    run_ = 1;
                    got = true;
                    break;
                }
                l->dead = true;
                dead_ = true;
            }
        }
        if (!got && n) {
            // Nothing anywhere: the current lane is as good as any, and a
            // burst left over is spent so the next pop starts a sweep.
            run_ = kBurst;
        }
        if (dead_) free_dead();
        return got;
    }

    // The consumer's copy of the registry, keeping its position on the
    // lane it was on if that lane is still there.
    void refresh() {
        lane* at = cur_ < lanes_.size() ? lanes_[cur_] : nullptr;
        {
            std::lock_guard<std::mutex> g(registry_m_);
            lanes_ = registry_;
            seen_version_ = version_.load(std::memory_order_relaxed);
        }
        place(at);
    }

    void place(lane* at) {
        auto it = std::find(lanes_.begin(), lanes_.end(), at);
        if (it != lanes_.end()) {
            cur_ = static_cast<std::size_t>(it - lanes_.begin());
        } else {
            cur_ = lanes_.empty() ? 0 : lanes_.size() - 1;  // the next sweep starts at 0
            run_ = kBurst;
        }
    }

    // Takes the dead lanes out of the registry and the copy, and frees them.
    void free_dead() {
        dead_ = false;
        lane* at = cur_ < lanes_.size() && !lanes_[cur_]->dead ? lanes_[cur_] : nullptr;
        std::vector<lane*> gone;
        auto is_dead = [](lane* l) { return l->dead; };
        {
            std::lock_guard<std::mutex> g(registry_m_);
            for (lane* l : registry_)
                if (l->dead) gone.push_back(l);
            registry_.erase(std::remove_if(registry_.begin(), registry_.end(), is_dead), registry_.end());
            version_.fetch_add(1, std::memory_order_release);
            lanes_ = registry_;
            seen_version_ = version_.load(std::memory_order_relaxed);
        }
        place(at);
        for (lane* l : gone) {
            laned_detail::lane_mark* m = l->mark;
            delete l;
            laned_detail::let_go(m);
            ++retired_;
        }
    }

    // How often a sweep looks at the exit marks of the lanes it finds
    // empty: often enough that the lanes of exited producers are freed
    // soon, rarely enough that a sweep reads one line per lane, not two.
    static constexpr std::uint64_t kExitEvery = 64;

#ifdef WTC_CHECK
    void claim_consumer() {
        thread_local const std::uintptr_t me = static_cast<std::uintptr_t>(pthread_self());
        std::uintptr_t seen = consumer_.load(std::memory_order_relaxed);
        if (seen == me) return;
        if (seen == 0 && consumer_.compare_exchange_strong(seen, me, std::memory_order_relaxed)) return;
        if (seen == me) return;
        std::fprintf(stderr,
                     "wtc: MPSC contract violated: a second consumer thread on queue %p "
                     "(first %#lx, now %#lx)\n",
                     static_cast<void*>(this), static_cast<unsigned long>(seen),
                     static_cast<unsigned long>(me));
        std::abort();
    }
    alignas(kLine) std::atomic<std::uintptr_t> consumer_{0};
#endif

    // Read by every producer on its first push and never written after.
    const std::uint64_t id_;

    // The registry: written under the mutex, by a producer's first push and
    // by the consumer when it frees lanes; the version is read by the
    // consumer once per sweep and changes only then.
    alignas(kLine) std::mutex registry_m_;
    std::vector<lane*> registry_;
    alignas(kLine) std::atomic<std::uint64_t> version_{0};

    // The consumer's own.
    alignas(kLine) std::vector<lane*> lanes_;
    std::size_t cur_ = 0;
    unsigned run_ = kBurst;
    bool dead_ = false;
    std::uint64_t seen_version_ = 0;
    std::uint64_t sweeps_ = 0;
    std::uint64_t retired_ = 0;
    char pad_[kLine];
};

}  // namespace wtc::rt
