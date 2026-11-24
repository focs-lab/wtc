// wtc::rt::index_queue: a bounded FIFO queue that never waits for another
// thread and never allocates after it is built, for any number of
// producers and consumers, over two lock-free rings of indices ("A
// scalable, portable, and memory-efficient lock-free FIFO queue", DISC
// 2019, the SCQ ring, used with the paper's indirection). Its pops and
// accepted pushes are linearisable; what a refused push means depends on
// the variant (Capacity, below).
//
// What it is for. The bounded ring (bounded_ring.hpp) is exact and fast,
// but a push that has taken its ticket and stops before filling its cell
// stops the pop of that ticket, and with many producers and one consumer
// that pop waits (blocking). Here a producer that stops anywhere inside its
// push stops nobody: the consumers take the other producers' elements past
// it, and a consumer that stops holds at most one cell. What it costs is
// two ring operations per queue operation, and a capacity that is not
// exact in the plain variant (below).
//
// The fact the compiler must prove: at most `threads` operations (the
// constructor's argument) are in flight on the queue at once, an operation
// that an element's constructor, move or destructor starts on this same
// queue counting apart from the one it runs in (so one thread can account
// for more than one), and the destructor runs after every operation has
// returned. The bound is the ring's: SCQ's threshold, which decides when a
// pop may answer empty, assumes no more threads than the ring has indices,
// and the ring is built with at least that many; the exact variant's spare
// cells are counted from it too. Built with WTC_CHECK, the queue counts the
// operations in flight and aborts when they are more than `threads`.
//
// The ring (scq_ring). A ring of N = 2n entries holds indices below n. Each
// entry is one 64-bit word: the cycle of the counter value that wrote it,
// a "safe" bit, and an index or one of two empty marks (kBot, written by a
// pop that arrived before its push; kBotC, written by the pop that consumed
// the entry). An enqueue takes a position T with a fetch-and-add of tail
// and writes its index into entry T mod N with a compare-and-swap, if the
// entry is empty and of an older cycle (and safe, or no pop has passed T);
// otherwise it takes another position. A dequeue takes a position H with a
// fetch-and-add of head; if the entry holds cycle(H), it consumes it (an
// atomic OR to kBotC); otherwise it marks the entry so that a late enqueue
// of an older cycle cannot use it (advancing an empty entry's cycle, or
// clearing an occupied one's safe bit), and answers empty if tail is not
// past H, or tries again. A threshold, reset to 3n - 1 by every enqueue and
// decremented by every failed dequeue, bounds how far dequeues can run
// ahead of the last enqueue: once it is negative the ring is empty, and
// that is what makes the ring lock-free rather than only obstruction-free.
// The ring needs N = 2n entries for n indices and the threshold 3n - 1 for
// the same reason (the paper's Section 5). The cycle field takes the bits
// above the index field and the safe bit, so a ring wraps its cycles after
// 2^63 positions of its head or its tail (an operation takes one or more),
// which this queue never reaches: centuries at a billion a second.
//
// The queue (index_queue). A data array of cells, a ring fq of free
// indices and a ring aq of allocated ones. A push takes an index from fq,
// constructs its element in that cell and enqueues the index in aq; a pop
// dequeues an index from aq, moves the element out, destroys it and
// returns the index to fq. FIFO is aq's order: a push takes effect at its
// enqueue in aq, a pop at its dequeue from aq.
//
// Capacity, the one difference from a ring under a mutex. A cell is out
// of fq from the push that takes it until the pop that empties it gives it
// back. So:
//
//   plain   capacity cells, and a push is refused when fq is empty: the
//           queue holds capacity elements, or fewer while pushes and pops
//           are in flight (each holds a cell). "Capacity minus the
//           operations in flight", what bounded_ring.hpp was written to
//           avoid; the right variant where the program only ever waits for
//           room (push_wait), which cannot tell the two apart except by
//           timing.
//   exact   a count of elements decides fullness: a push increments it,
//           unless it is at the capacity, before it takes a cell; a pop
//           decrements it as soon as it has dequeued its index, before it
//           moves the element out. The data array has capacity + threads
//           - 1 cells, so a push that passed the count always finds a
//           cell: fewer than the capacity are held by elements and the
//           other pushes that passed it, and fewer than threads by pops
//           still moving out (the push is not one of them). A pop held inside its
//           move does not make the queue look full, as it does not in the
//           mutex ring. What remains: the count includes pushes that have
//           passed it and not yet enqueued their index, so a push may be
//           refused while such a push is in flight, and a pop meanwhile may
//           answer empty; under the mutex the in-flight push would have
//           held the lock and the pop waited. So the histories are those
//           of a queue whose push takes effect in two steps: it reserves a
//           place (the count) and later publishes its element (aq). Pops
//           and accepted pushes are linearisable as a FIFO queue that never
//           holds more than the capacity; a push is refused only at an
//           instant when the elements queued and the places reserved are
//           the capacity. A pop, likewise, gives its place back in a second
//           step: it takes its element when it dequeues its index and
//           decrements the count after its ring operation returns, so a
//           place also stays reserved for
//           a pop between the two, and a push may be refused then with
//           nothing queued. That is not linearisable as a bounded queue:
//           with capacity 1 and a push stopped after its reservation, a
//           refused push and then an empty pop on one thread have no order
//           (index_queue_test inflight), and the same with a pop stopped
//           between its two steps. A linearisable exact queue that
//           never waits exists -- a wait-free universal construction gives
//           one, in which other threads finish a stopped push for it -- but
//           this is not one: a push builds its element after its
//           reservation, and no other thread can publish it.
//
// Progress. Every operation is lock-free: each loop retries only because
// another operation took the position or changed the entry it read, or
// because the ring's threshold has not yet run out; nothing waits for a
// thread. A push or pop allocates nothing; the element's own constructor,
// move and destructor run inside them. The wait-free form of the same
// ring is wcq (commit "Add the wait-free slow path to the index ring").
#pragma once

// With WTC_TEST_RING_MUTANT, a deliberate bug that runtime/test/model must
// catch: 1, a dequeue that does not mark the entry it passes; 2, an
// enqueue that ignores the safe bit. Nothing otherwise.
#ifndef WTC_TEST_RING_MUTANT
#define WTC_TEST_RING_MUTANT 0
#endif

// The C headers, and the C functions unqualified: the model checker's
// runtime (runtime/test/model) supplies its own C library and only that.
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace wtc::rt {

// Points where a test may hold a thread or count steps; no_ring_hooks, the
// default, does nothing.
struct no_ring_hooks {
    // In a ring's enqueue, after its fetch-and-add of tail and before it
    // reads the entry; in a dequeue, the same after head.
    static void enq_reserved() {}
    static void deq_reserved() {}
    // In the queue's push, after it has taken a cell from fq and before
    // the element is constructed and the index enqueued in aq.
    static void push_has_cell() {}
    // In the queue's pop, after it has dequeued its index from aq and
    // before it moves the element out.
    static void pop_has_cell() {}
};

template <class Hooks = no_ring_hooks>
class scq_ring {
public:
    using u64 = std::uint64_t;

    // A ring for indices below 2^order (order >= 1), holding the first
    // `full` of them (0, 1, ...) when built, or none.
    scq_ring(unsigned order, std::uint32_t full)
        : order_(order),
          bits_(order + 1),
          size_(u64{1} << (order + 1)),
          entries_(static_cast<std::atomic<u64>*>(malloc(sizeof(std::atomic<u64>) * size_))) {
        if (!entries_) abort();
        if (order < 1 || order > 30 || full > (u64{1} << order)) {
            fprintf(stderr, "wtc: index ring of order %u with %u indices\n", order, full);
            abort();
        }
        for (u64 j = 0; j < size_; ++j) ::new (&entries_[j]) std::atomic<u64>(make(0, 1, bot()));
        // Positions start at N, cycle 1, so the entries' cycle 0 is older.
        for (u64 i = 0; i < full; ++i)
            entries_[remap(i)].store(make(1, 1, i), std::memory_order_relaxed);
        head_.store(size_, std::memory_order_relaxed);
        tail_.store(size_ + full, std::memory_order_relaxed);
        threshold_.store(full ? threshold_max() : -1, std::memory_order_seq_cst);
    }

    // The array comes from malloc rather than new: the model checker's
    // runtime has malloc and no C++ allocator.
    ~scq_ring() { free(entries_); }

    scq_ring(const scq_ring&) = delete;
    scq_ring& operator=(const scq_ring&) = delete;

    // Indices this ring may hold: n.
    std::uint32_t indices() const { return static_cast<std::uint32_t>(u64{1} << order_); }

    // Enqueues idx < n. The caller holds idx, and at most n indices are in
    // the ring or held, so the ring is never full.
    void enqueue(std::uint32_t idx) {
        while (!try_enq(idx)) {
        }
    }

    // Dequeues the oldest index; false when the ring is empty.
    bool dequeue(std::uint32_t& idx) {
        if (threshold_.load(std::memory_order_seq_cst) < 0) return false;
        for (;;) {
            switch (try_deq(idx)) {
            case kOk: return true;
            case kEmpty: return false;
            default: break;
            }
        }
    }

private:
    enum result { kOk, kEmpty, kRetry };

    u64 bot() const { return size_ - 2; }   // a pop arrived before the push
    u64 botc() const { return size_ - 1; }  // consumed; all the index bits set
    std::int64_t threshold_max() const { return 3 * static_cast<std::int64_t>(u64{1} << order_) - 1; }

    u64 make(u64 cycle, u64 safe, u64 index) const { return cycle << (bits_ + 1) | safe << bits_ | index; }
    u64 cycle_of(u64 e) const { return e >> (bits_ + 1); }
    u64 safe_of(u64 e) const { return e >> bits_ & 1; }
    u64 index_of(u64 e) const { return e & (size_ - 1); }
    u64 cycle(u64 position) const { return position >> bits_; }

    // Spreads adjacent positions over different lines, eight entries to a
    // line, so that consecutive operations do not share one.
    u64 remap(u64 position) const {
        const u64 p = position & (size_ - 1);
        if (bits_ < 4) return p;
        return (p & 7) << (bits_ - 3) | p >> 3;
    }

    bool try_enq(std::uint32_t idx) {
        const u64 t = tail_.fetch_add(1, std::memory_order_seq_cst);
        Hooks::enq_reserved();
        std::atomic<u64>& slot = entries_[remap(t)];
        u64 e = slot.load(std::memory_order_seq_cst);
        for (;;) {
            const u64 ix = index_of(e);
#if WTC_TEST_RING_MUTANT == 2
            const bool safe = true;  // runtime/test/model: an enqueue that ignores the safe bit
#else
            const bool safe = safe_of(e) || head_.load(std::memory_order_seq_cst) <= t;
#endif
            if (cycle_of(e) < cycle(t) && (ix == bot() || ix == botc()) && safe) {
                if (!slot.compare_exchange_strong(e, make(cycle(t), 1, idx), std::memory_order_seq_cst))
                    continue;  // e reloaded: decide again
                if (threshold_.load(std::memory_order_seq_cst) != threshold_max())
                    threshold_.store(threshold_max(), std::memory_order_seq_cst);
                return true;
            }
            return false;  // a later cycle, occupied, or unsafe: take another position
        }
    }

    result try_deq(std::uint32_t& idx) {
        const u64 h = head_.fetch_add(1, std::memory_order_seq_cst);
        Hooks::deq_reserved();
        std::atomic<u64>& slot = entries_[remap(h)];
        u64 e = slot.load(std::memory_order_seq_cst);
        for (;;) {
            const u64 ec = cycle_of(e);
            if (ec == cycle(h)) {
                // The enqueue of position h wrote it: consume.
                slot.fetch_or(botc(), std::memory_order_seq_cst);
                idx = static_cast<std::uint32_t>(index_of(e));
                return kOk;
            }
            if (ec >= cycle(h)) break;  // a later cycle: nothing to mark
            // Keep an enqueue of an older cycle out of this entry: an empty
            // one moves on to cycle(h); an occupied one (an element of an
            // older cycle, still to be dequeued by its own pop) loses its
            // safe bit.
            const u64 ix = index_of(e);
            const u64 n = ix == bot() || ix == botc() ? make(cycle(h), safe_of(e), bot()) : make(ec, 0, ix);
#if WTC_TEST_RING_MUTANT == 1
            break;  // runtime/test/model: a dequeue that marks nothing
#endif
            if (slot.compare_exchange_strong(e, n, std::memory_order_seq_cst)) break;
        }
        const u64 t = tail_.load(std::memory_order_seq_cst);
        if (t <= h + 1) {
            catchup(t, h + 1);
            threshold_.fetch_sub(1, std::memory_order_seq_cst);
            return kEmpty;
        }
        if (threshold_.fetch_sub(1, std::memory_order_seq_cst) <= 0) return kEmpty;
        return kRetry;
    }

    // Moves tail up to head after dequeues ran past it, so that the next
    // enqueue does not take positions every dequeue has already passed.
    void catchup(u64 t, u64 h) {
        while (!tail_.compare_exchange_strong(t, h, std::memory_order_seq_cst)) {
            h = head_.load(std::memory_order_seq_cst);
            t = tail_.load(std::memory_order_seq_cst);
            if (t >= h) break;
        }
    }

    const unsigned order_;
    const unsigned bits_;  // the index field: log2 N
    const u64 size_;       // N = 2n entries
    std::atomic<u64>* const entries_;
    alignas(64) std::atomic<u64> head_{0};
    alignas(64) std::atomic<u64> tail_{0};
    alignas(64) std::atomic<std::int64_t> threshold_{-1};
    char pad_[64 - sizeof(std::atomic<std::int64_t>)];
};

template <class T, class Hooks = no_ring_hooks, template <class> class Ring = scq_ring>
class index_queue {
public:
    static constexpr std::size_t kMaxCapacity = std::size_t{1} << 28;
    static constexpr unsigned kDefaultThreads = 128;

    // capacity: the bound; exact: the variant (see the top); threads: at
    // most this many operations in flight at once, nested ones counted
    // apart (see the top).
    index_queue(std::size_t capacity, bool exact, unsigned threads = kDefaultThreads)
        : cap_(static_cast<std::uint32_t>(capacity)),
          exact_(exact),
          threads_(threads),
          cells_n_(static_cast<std::uint32_t>(capacity + (exact && threads ? threads - 1 : 0))),
          order_(order_for(cells_n_ > threads ? cells_n_ : threads)),
          fq_(order_, cells_n_),
          aq_(order_, 0) {
        if (capacity > kMaxCapacity || threads == 0) {
            fprintf(stderr, "wtc: index queue of capacity %zu for %u threads\n", capacity, threads);
            abort();
        }
        const std::size_t bytes = sizeof(cell) * (cells_n_ ? cells_n_ : 1);
        cells_ = static_cast<cell*>(alignof(cell) <= alignof(std::max_align_t)
                                        ? malloc(bytes)
                                        : aligned_alloc(alignof(cell), (bytes + alignof(cell) - 1) /
                                                                           alignof(cell) * alignof(cell)));
        if (!cells_) abort();
    }

    ~index_queue() {
        std::uint32_t i;
        while (aq_.dequeue(i)) cells_[i].elem()->~T();
        free(cells_);
    }

    index_queue(const index_queue&) = delete;
    index_queue& operator=(const index_queue&) = delete;

    std::size_t capacity() const { return cap_; }
    bool exact() const { return exact_; }

    // Appends v unless the queue is full; false, and v untouched, if it is.
    template <class V>
    bool try_push(V&& v) noexcept {
#ifdef WTC_CHECK
        const in_flight check(*this);
#endif
        if (exact_) {
            std::int64_t c = count_.load(std::memory_order_seq_cst);
            do {
                if (c >= static_cast<std::int64_t>(cap_)) return false;
            } while (!count_.compare_exchange_strong(c, c + 1, std::memory_order_seq_cst));
        }
        std::uint32_t i;
        if (!fq_.dequeue(i)) {
            if (!exact_) return false;
            // Cannot happen with at most `threads` operations in flight:
            // the count leaves a cell for every push that passed it. Built
            // with WTC_CHECK the queue aborts earlier, as soon as one
            // operation more is in flight.
            fprintf(stderr, "wtc: index queue %p: no free cell; more operations in flight than it was built for\n",
                         static_cast<void*>(this));
            abort();
        }
        Hooks::push_has_cell();
        ::new (static_cast<void*>(cells_[i].raw)) T(std::forward<V>(v));
        aq_.enqueue(i);
        return true;
    }

    // Moves the oldest element to f(T&&) and returns true; false when the
    // queue is empty.
    template <class F>
    bool consume(F&& f) noexcept {
#ifdef WTC_CHECK
        const in_flight check(*this);
#endif
        std::uint32_t i;
        if (!aq_.dequeue(i)) return false;
        if (exact_) count_.fetch_sub(1, std::memory_order_seq_cst);
        Hooks::pop_has_cell();
        T* e = cells_[i].elem();
        f(std::move(*e));
        e->~T();
        fq_.enqueue(i);
        return true;
    }

    bool try_pop(T& out) noexcept {
        return consume([&](T&& v) noexcept { out = std::move(v); });
    }

    // The exact variant's count: elements queued and pushes that have
    // passed the count, at one instant. Not kept by the plain variant.
    std::size_t size() const noexcept {
        const std::int64_t c = count_.load(std::memory_order_seq_cst);
        return c > 0 ? static_cast<std::size_t>(c) : 0;
    }

private:
    struct cell {
        alignas(T) unsigned char raw[sizeof(T)];
        T* elem() { return std::launder(reinterpret_cast<T*>(raw)); }
    };

#ifdef WTC_CHECK
    // The operations in flight, counted on the way in and out, nested ones
    // (an element's constructor pushing to this queue) included, as they
    // hold cells too.
    struct in_flight {
        index_queue& q;
        explicit in_flight(index_queue& q) : q(q) {
            const unsigned n = q.inflight_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n > q.threads_) {
                fprintf(stderr,
                        "wtc: index queue contract violated: %u operations in flight on queue %p, built for "
                        "%u threads\n",
                        n, static_cast<void*>(&q), q.threads_);
                abort();
            }
        }
        ~in_flight() { q.inflight_.fetch_sub(1, std::memory_order_relaxed); }
    };
    std::atomic<unsigned> inflight_{0};
#endif

    static unsigned order_for(std::uint32_t n) {
        unsigned o = 1;
        while ((std::uint64_t{1} << o) < n) ++o;
        return o;
    }

    const std::uint32_t cap_;
    const bool exact_;
    const unsigned threads_;
    const std::uint32_t cells_n_;
    const unsigned order_;
    cell* cells_ = nullptr;
    Ring<Hooks> fq_;
    Ring<Hooks> aq_;
    alignas(64) std::atomic<std::int64_t> count_{0};
    char pad_[64 - sizeof(std::atomic<std::int64_t>)];
};

}  // namespace wtc::rt
