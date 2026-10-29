// wtc::rt::bounded_ring: a bounded linearisable FIFO queue whose capacity
// is exact, for the queues programs bound themselves (spdlog's
// mpmc_blocking_queue over circular_q): a push that finds the queue full
// fails, and a push that overruns removes the oldest element and appends
// its own in one step. The waiting forms (wait while full, wait while
// empty) are the shim's (shim/bounded.hpp, runtime/waiter.hpp); this file
// is the queue alone, and none of its operations sleeps.
//
// The contract, stated against the original, a ring under a mutex:
//
//   - The queue never holds more than `capacity` elements, and a push
//     fails (or, in the shim, waits) only when, at a point inside the
//     call, it holds exactly `capacity`. Not "capacity plus the operations
//     in flight", and not less: a push and a pop each take effect at one
//     atomic step on the queue's counters, the same step that decides
//     whether they may, so the sequence of those steps is a sequence of
//     operations on a sequential bounded queue and every answer is the
//     one that queue gives. (The argument is in docs/lifting.md, "The
//     bound".)
//   - FIFO over all producers, not only per producer: the element a pop
//     returns is the one whose push took the ticket the pop takes.
//   - A push that overruns, when the queue is full, removes the oldest
//     element and appends its own at one step, so two such pushes overrun
//     two elements, never three, whatever runs between them; at capacity
//     zero it drops its own element, as the original does.
//   - size() is exact: the counters at one instant.
//   - The count of overruns is approximate while an overrun is in flight:
//     it is incremented just after the step that overran, so a reader can
//     see the queue after an overrun and the count before it. It is exact
//     once the pushes have returned.
//
// Design. Two counters of tickets, head (pops taken) and tail (pushes
// taken), and a ring of R cells, R the least power of two at or above the
// capacity, and at least two. Ticket i is cell i mod R. Each cell carries a
// sequence number, as in Vyukov's bounded queue: i when it is free for the
// push of ticket i, i + 1 once that push has filled it, i + R once the pop
// of ticket i has emptied it, which is the next lap's push's ticket.
//
// What differs from Vyukov's queue is where fullness and emptiness are
// decided. There a push is refused when its cell is still occupied, which
// it also is while a pop that has taken that cell's ticket is still moving
// the element out: a pop preempted at that point makes the queue look full
// to every producer, although its element has left the queue. That is
// "capacity minus the pops in flight", and it is observable: a discarding
// push would discard where the original had room. Here a push compares
// tail with head, the pops taken, not finished, and is refused only when
// tail - head >= capacity; once it has its ticket it waits, if it must,
// for the pop of the previous lap to finish with the cell. Symmetrically a
// pop is refused only when head == tail, and once it has its ticket it
// waits for the push of that ticket to fill the cell. Those waits are on
// operations already in progress, never on one that has not started.
//
// Two layouts of the counters.
//
//   split   head and tail are separate words on separate lines. A push
//           reads tail, then head, and takes its ticket with a
//           compare-and-swap of tail (a store, with one producer); a pop
//           reads head, then tail, and takes its ticket with a compare-
//           and-swap of head (a store, with one consumer). The second read
//           is what makes a pop's refusal exact: at the moment it is made
//           head cannot have moved backwards, so tail - head was at least
//           what the reads give. A push refuses only once a third read
//           finds tail where the first did: with several producers, others
//           may push and consumers pop past the tail it read before it
//           reads head, and the difference of the two reads is then no
//           count at all. The consumers keep tail as they
//           last read it, a value tail has only grown from: an element by
//           that copy is an element, and only an apparent empty queue reads
//           tail itself. Used unless the queue overruns.
//   packed  head and tail are the two halves of one 64-bit word, and every
//           operation is one compare-and-swap of it. An overrun must take
//           the oldest element's ticket and its own at once; with two
//           words a push could take the slot an overrun freed before the
//           overrun had pushed, and the queue would be, for a moment, at
//           capacity + 1, which a discarding push or size() would see.
//
// Tickets are 32-bit and wrap; every comparison is of differences, which
// stay below 2^31 because the capacity is at most 2^30.
//
// The sides. With one producer (or one consumer) its counter is written
// by a plain store; the compiler proves the side single before it says
// so, and with WTC_CHECK the queue checks it, as the other queues do. The
// packed layout takes every ticket with a compare-and-swap whatever the
// sides, since an overrun makes every producer a consumer as well.
//
// Elements. A push constructs its element in the cell from its argument
// (by move or copy) only after it has its ticket, so a failed push leaves
// its argument untouched; a pop hands the element to its taker and
// destroys it in the cell. The element's move and destructor must not
// throw: a ticket taken is a promise to fill or empty a cell, and an
// exception between would leave the next lap waiting for ever, so the
// program ends instead (the shim's queues do the same). The destructor
// destroys the elements still queued and must run after every operation
// has returned.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

#ifdef WTC_CHECK
#include <pthread.h>
#endif

// With WTC_TEST_PUSH_STEPS, a push of the split layout calls
// wtc_test_push_step between reading tail and reading head, where
// runtime/test/bounded_test.cpp holds it while other threads move both
// counters. Nothing otherwise.
#ifdef WTC_TEST_PUSH_STEPS
extern "C" void wtc_test_push_step();
#define WTC_TEST_PUSH_STEP() wtc_test_push_step()
#else
#define WTC_TEST_PUSH_STEP() ((void)0)
#endif

namespace wtc::rt {

template <class T, unsigned kLine = 64>
class bounded_ring {
public:
    static constexpr std::size_t kMaxCapacity = std::size_t{1} << 30;

    // manyProducers / manyConsumers: whether more than one thread may push
    // / pop over the queue's life. overrun: whether push_overrun may be
    // called, which chooses the packed layout.
    bounded_ring(std::size_t capacity, bool manyProducers, bool manyConsumers, bool overrun)
        : cap_(static_cast<std::uint32_t>(capacity)),
          mask_(ringSize(capacity) - 1),
          packed_(overrun),
          mp_(manyProducers || overrun),
          mc_(manyConsumers || overrun) {
        if (capacity > kMaxCapacity) {
            std::fprintf(stderr, "wtc: bounded queue of capacity %zu: at most %zu\n", capacity, kMaxCapacity);
            std::abort();
        }
        cells_ = static_cast<cell*>(::operator new(sizeof(cell) * (mask_ + 1), std::align_val_t{alignof(cell)}));
        for (std::uint32_t i = 0; i <= mask_; ++i) ::new (&cells_[i]) cell(i);
    }

    ~bounded_ring() {
        std::uint32_t h, t;
        if (packed_) {
            const std::uint64_t s = state_.load(std::memory_order_relaxed);
            h = headOf(s);
            t = tailOf(s);
        } else {
            h = head_.load(std::memory_order_relaxed);
            t = tail_.load(std::memory_order_relaxed);
        }
        for (; h != t; ++h) cells_[h & mask_].elem()->~T();
        for (std::uint32_t i = 0; i <= mask_; ++i) cells_[i].~cell();
        ::operator delete(cells_, std::align_val_t{alignof(cell)});
    }

    bounded_ring(const bounded_ring&) = delete;
    bounded_ring& operator=(const bounded_ring&) = delete;

    std::size_t capacity() const { return cap_; }
    bool overruns() const { return packed_; }

    // Appends v (moved from when it is an rvalue) unless the queue is full;
    // false, and v untouched, when it is.
    template <class V>
    bool try_push(V&& v) noexcept {
#ifdef WTC_CHECK
        if (!mp_) claim(producer_, "producer");
#endif
        std::uint32_t t;
        if (packed_) {
            std::uint64_t s = state_.load(std::memory_order_seq_cst);
            for (;;) {
                t = tailOf(s);
                if (t - headOf(s) >= cap_) return false;
                if (state_.compare_exchange_weak(s, pack(headOf(s), t + 1), std::memory_order_seq_cst)) break;
            }
        } else {
            for (;;) {
                t = tail_.load(std::memory_order_seq_cst);
                WTC_TEST_PUSH_STEP();
                const std::uint32_t h = head_.load(std::memory_order_seq_cst);
                if (t - h >= cap_) {
                    // Exact only if tail still held t when head was read:
                    // other producers may have pushed, and consumers popped
                    // past t, in between, and then h is beyond t and t - h
                    // wraps. A tail read again after head that still equals
                    // t was t at that read (tickets do not come round in
                    // between), so t - h is the count at one instant, and
                    // at least the capacity.
                    if (tail_.load(std::memory_order_seq_cst) != t) continue;
                    return false;
                }
                if (!mp_) {
                    tail_.store(t + 1, std::memory_order_seq_cst);
                    break;
                }
                if (tail_.compare_exchange_weak(t, t + 1, std::memory_order_seq_cst)) break;
            }
        }
        fill(t, std::forward<V>(v));
        return true;
    }

    // Appends v; when the queue is full, removes the oldest element first,
    // at the same step, and returns true. At capacity zero v itself is
    // dropped. Only on a queue built with overrun.
    template <class V>
    bool push_overrun(V&& v) noexcept {
        if (!packed_) {
            std::fprintf(stderr,
                         "wtc: an overrunning push on bounded queue %p, which the compiler built without "
                         "one\n",
                         static_cast<void*>(this));
            std::abort();
        }
        if (cap_ == 0) {
            overruns_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        std::uint64_t s = state_.load(std::memory_order_seq_cst);
        for (;;) {
            const std::uint32_t h = headOf(s), t = tailOf(s);
            const bool full = t - h >= cap_;
            if (state_.compare_exchange_weak(s, pack(full ? h + 1 : h, t + 1), std::memory_order_seq_cst)) {
                if (full) {
                    // The oldest first: at R == capacity it is the cell
                    // this push fills.
                    take(h, [](T&&) noexcept {});
                    overruns_.fetch_add(1, std::memory_order_relaxed);
                }
                fill(t, std::forward<V>(v));
                return full;
            }
        }
    }

    // Moves the oldest element to f(T&&) and returns true; false when the
    // queue is empty.
    template <class F>
    bool consume(F&& f) noexcept {
#ifdef WTC_CHECK
        if (!mc_) claim(consumer_, "consumer");
#endif
        std::uint32_t h;
        if (packed_) {
            std::uint64_t s = state_.load(std::memory_order_seq_cst);
            for (;;) {
                h = headOf(s);
                if (h == tailOf(s)) return false;
                if (state_.compare_exchange_weak(s, pack(h + 1, tailOf(s)), std::memory_order_seq_cst)) break;
            }
        } else {
            for (;;) {
                h = head_.load(std::memory_order_seq_cst);
                // A tail read earlier is a lower bound of tail, so an element
                // by it is an element; only an apparent lack of one reads
                // tail again, after head, as the refusal must. Acquire and
                // release: the tail this copy was read from then happens
                // before this pop's step, as the argument needs.
                if (static_cast<std::int32_t>(tail_seen_.load(std::memory_order_acquire) - h) <= 0) {
                    const std::uint32_t t = tail_.load(std::memory_order_seq_cst);
                    tail_seen_.store(t, std::memory_order_release);
                    if (h == t) return false;
                }
                if (!mc_) {
                    head_.store(h + 1, std::memory_order_seq_cst);
                    break;
                }
                if (head_.compare_exchange_weak(h, h + 1, std::memory_order_seq_cst)) break;
            }
        }
        take(h, f);
        return true;
    }

    // The number of elements at one instant: pushes taken minus pops
    // taken. In the split layout, head read twice around tail: equal reads
    // mean head held that value when tail was read.
    std::size_t size() const noexcept {
        if (packed_) {
            const std::uint64_t s = state_.load(std::memory_order_seq_cst);
            return tailOf(s) - headOf(s);
        }
        for (;;) {
            const std::uint32_t h = head_.load(std::memory_order_seq_cst);
            const std::uint32_t t = tail_.load(std::memory_order_seq_cst);
            if (head_.load(std::memory_order_seq_cst) == h) return t - h;
        }
    }

    std::size_t overrun_counter() const noexcept { return overruns_.load(std::memory_order_relaxed); }
    void reset_overrun_counter() noexcept { overruns_.store(0, std::memory_order_relaxed); }

private:
    struct cell {
        explicit cell(std::uint32_t i) : seq(i) {}
        std::atomic<std::uint32_t> seq;
        alignas(T) unsigned char raw[sizeof(T)];
        T* elem() { return std::launder(reinterpret_cast<T*>(raw)); }
    };

    // At least two cells: with one, "filled for ticket t" (t + 1) and "free
    // for ticket t + 1" (t + R) would be the same sequence number.
    static std::uint32_t ringSize(std::size_t capacity) {
        std::uint32_t r = 2;
        while (r < capacity && r < kMaxCapacity) r <<= 1;
        return r;
    }
    static std::uint32_t headOf(std::uint64_t s) { return static_cast<std::uint32_t>(s >> 32); }
    static std::uint32_t tailOf(std::uint64_t s) { return static_cast<std::uint32_t>(s); }
    static std::uint64_t pack(std::uint32_t h, std::uint32_t t) { return std::uint64_t{h} << 32 | t; }

    static void relax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    // The push of ticket t: waits for the previous lap's pop to have
    // emptied the cell, then fills it.
    template <class V>
    void fill(std::uint32_t t, V&& v) noexcept {
        cell& c = cells_[t & mask_];
        while (c.seq.load(std::memory_order_acquire) != t) relax();
        ::new (static_cast<void*>(c.raw)) T(std::forward<V>(v));
        c.seq.store(t + 1, std::memory_order_release);
    }

    // The pop of ticket h: waits for its push to have filled the cell, then
    // hands the element out and frees the cell for the next lap.
    template <class F>
    void take(std::uint32_t h, F&& f) noexcept {
        cell& c = cells_[h & mask_];
        while (c.seq.load(std::memory_order_acquire) != h + 1) relax();
        T* e = c.elem();
        f(std::move(*e));
        e->~T();
        c.seq.store(h + mask_ + 1, std::memory_order_release);
    }

#ifdef WTC_CHECK
    void claim(std::atomic<std::uintptr_t>& side, const char* what) {
        thread_local const std::uintptr_t me = static_cast<std::uintptr_t>(pthread_self());
        std::uintptr_t seen = side.load(std::memory_order_relaxed);
        if (seen == me) return;
        if (seen == 0 && side.compare_exchange_strong(seen, me, std::memory_order_relaxed)) return;
        if (seen == me) return;
        std::fprintf(stderr,
                     "wtc: bounded queue contract violated: a second %s thread on queue %p (first %#lx, "
                     "now %#lx)\n",
                     what, static_cast<void*>(this), static_cast<unsigned long>(seen),
                     static_cast<unsigned long>(me));
        std::abort();
    }
    std::atomic<std::uintptr_t> producer_{0}, consumer_{0};
#endif

    // Read-only after construction.
    const std::uint32_t cap_;
    const std::uint32_t mask_;
    const bool packed_, mp_, mc_;
    cell* cells_ = nullptr;
    // Split layout: the consumers' line, with tail as a consumer last read
    // it, and the producers' line. The copy only lags, and is refreshed when
    // it makes the queue look empty, which keeps a pop from reading the
    // producers' line on every operation. The producers keep no such copy
    // of head: when the queue is full, as it is whenever the consumer is the
    // slower side, every push would refresh it, a store on the producers'
    // contended line, which measured slower than reading head.
    alignas(kLine) std::atomic<std::uint32_t> head_{0};
    std::atomic<std::uint32_t> tail_seen_{0};
    alignas(kLine) std::atomic<std::uint32_t> tail_{0};
    // Packed layout: head in the high half, tail in the low.
    alignas(kLine) std::atomic<std::uint64_t> state_{0};
    alignas(kLine) std::atomic<std::size_t> overruns_{0};
    char pad_[kLine];
};

}  // namespace wtc::rt
