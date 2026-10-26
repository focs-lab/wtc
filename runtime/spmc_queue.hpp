// wtc::rt::spmc_queue: an unbounded linearisable FIFO queue for one producer
// and any number of consumers, over a list of array segments whose cells the
// consumers claim with a compare-and-swap.
//
// The fact the compiler must prove before substituting this for wtc::queue:
//
//   Over the object's whole lifetime exactly one thread calls push (or
//   push_deferred and publish), any number call try_pop, and the
//   destructor runs after all of them have made their last call (ordered
//   by a join or equivalent).
//
// What breaks if that fact is false: two producers construct into the same
// cell and publish each other's counts, so elements are lost, duplicated or
// read half-built. Nothing is detected at runtime unless WTC_CHECK is
// defined, and TSan reports it only on interleavings where it occurs.
//
// Semantics preserved from the generic queue (decisions/0003): every push
// happens-before the pop that returns its element; linearisable and FIFO in
// the order of the pushes; push never blocks and never fails; no capacity;
// try_pop returns false only when every element published so far has been
// popped. A consumer never waits for another thread: its compare-and-swap
// fails only because another consumer claimed the cell, so some pop always
// completes (lock-free), and it never reads a cell the producer has not
// published, so nothing is skipped and nothing is waited for.
//
// Design. The producer is the only writer of the tail segment: it constructs
// an element in the next cell and publishes the cells it has filled with one
// release store of the segment's count (per push, or per run with
// push_deferred and publish). A full segment is published whole before the
// next one is linked, so a consumer that finds a segment drained and no next
// one has found the queue empty. Consumers share the head segment's claim
// index and take the cell at it with a compare-and-swap against the
// published count; the element is then moved out of a cell no other thread
// touches. A consumer that reaches the end of a drained segment moves the
// head on and retires the segment through hazard pointers (hazard.hpp),
// which every consumer holds on the segment it reads.
//
// No consumer waits for another: the program's code a pop runs (the
// element's move or copy, the shared_ptr's allocation in the shim) runs
// after the claim, holding nothing another thread waits on. A move that
// takes a lock another consumer holds while it pops therefore cannot
// deadlock here, as it cannot on the general queue, whose consumers do not
// wait for each other either.
//
// What this replaces, and why: the queue for this topology used to be the
// spsc ring with its consumers taking turns under a spin lock. Every pop then
// moved the lock's line, the ring's consumer state and the element's line
// from the previous consumer or the producer while holding the lock, so the
// consumers' pops ran one after another at the speed of three or four line
// transfers each; with eight workers and tasks of a microsecond the pool ran
// at 0.4-0.5x the general queue (benchmarks/results/2026-10-26-eval-pool).
// Here only the claim, one compare-and-swap on one line, is serial; the
// element is read in parallel, and an empty poll writes nothing.
//
// To keep consumers off the producer's line, the consumers remember on their
// own line the highest published count one of them has read, and read the
// producer's count only when they have claimed up to it. The producer's
// store then usually finds its line still its own.
//
// The shim instantiates it with Slot = T, the element by value. Elements
// cannot be lent: a borrowed element would be destroyed in place when the
// segment is retired, which no consumer can see coming.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

#include "hazard.hpp"

#ifdef WTC_CHECK
#include <pthread.h>
#endif

namespace wtc::rt {

template <class Slot, unsigned kSegment = 1024, unsigned kLine = 64>
class spmc_queue {
    static_assert(kSegment >= 2, "a segment needs at least two cells");
    enum : unsigned { kHead = hazard::kPopSlot };

    struct cell {
        alignas(Slot) unsigned char raw[sizeof(Slot)];
        Slot* slot() { return std::launder(reinterpret_cast<Slot*>(raw)); }
    };

    struct segment {
        // The consumers' line: the next cell to claim, and the highest
        // published count a consumer has read, never above enq.
        alignas(kLine) std::atomic<std::uint64_t> deq{0};
        std::atomic<std::uint64_t> seen{0};
        // The producer's line: cells published.
        alignas(kLine) std::atomic<std::uint64_t> enq{0};
        alignas(kLine) std::atomic<segment*> next{nullptr};
        alignas(kLine) cell cells[kSegment];
    };

    static void free_segment(void* p) { delete static_cast<segment*>(p); }

 public:
    spmc_queue() {
        segment* s = new segment;
        head_.store(s, std::memory_order_relaxed);
        tail_ = s;
    }

    ~spmc_queue() {
        segment* s = head_.load(std::memory_order_relaxed);
        std::uint64_t from = s->deq.load(std::memory_order_relaxed);
        while (s) {
            segment* next = s->next.load(std::memory_order_relaxed);
            // The tail's cells past its published count were filled by
            // push_deferred and never published; they hold elements too.
            const std::uint64_t to = s == tail_ ? filled_ : s->enq.load(std::memory_order_relaxed);
            for (std::uint64_t i = from; i < to; ++i) s->cells[i].slot()->~Slot();
            delete s;
            s = next;
            from = 0;
        }
    }

    spmc_queue(const spmc_queue&) = delete;
    spmc_queue& operator=(const spmc_queue&) = delete;

    template <class S>
    void push(S&& s) {
        push_deferred(std::forward<S>(s));
        publish();
    }

    // Fills the next cell without publishing it: the elements filled since
    // the last publish become visible together at the next one, or when
    // their segment fills, whichever comes first, and always in order.
    template <class S>
    void push_deferred(S&& s) {
#ifdef WTC_CHECK
        claim_producer();
#endif
        if (filled_ == kSegment) {
            segment* fresh = new segment;
            // The full segment is published whole before the next one is
            // linked: a consumer that sees the link has every cell of it.
            tail_->enq.store(kSegment, std::memory_order_release);
            tail_->next.store(fresh, std::memory_order_release);
            tail_ = fresh;
            filled_ = 0;
        }
        ::new (static_cast<void*>(tail_->cells[filled_].raw)) Slot(std::forward<S>(s));
        ++filled_;
    }

    void publish() { tail_->enq.store(filled_, std::memory_order_release); }

    // Pops the oldest element into f, which is called once, with the
    // element as an rvalue still in its cell, or not at all when the queue
    // was empty; the cell is released after f returns. f may push to any
    // queue but must not pop from one: it runs while the pop's hazard slot
    // (hazard.hpp, kPopSlot) is all that keeps the segment alive.
    template <class F>
    bool consume(F&& f) {
        static_assert(std::is_nothrow_invocable_v<F, Slot&&>,
                      "the taker runs with the element still in its cell, which is released after "
                      "it returns; it must not throw");
        return pop(hazard::mine(), f);
    }

    bool try_pop(Slot& out) {
        return consume([&](Slot&& s) noexcept { out = std::move(s); });
    }

 private:
    // The head segment, protected. A consumer's slot keeps holding it from
    // one pop to the next, so that a pop finds it protected already and
    // pays a load rather than the sequentially consistent store: a segment
    // the slot has held since it was protected cannot have been freed, so
    // finding it still the head means it is still protected. The price is
    // that a segment retired while a consumer is idle is freed at that
    // consumer's next pop or its exit: one segment per consumer thread.
    segment* held_head(hazard::record& hp) {
        segment* h = head_.load(std::memory_order_seq_cst);
        if (hp.slot[kHead].load(std::memory_order_relaxed) == h) return h;
        return hazard::protect(hp, kHead, head_);
    }

    template <class F>
    bool pop(hazard::record& hp, F& f) {
        for (;;) {
            segment* h = held_head(hp);
            std::uint64_t d = h->deq.load(std::memory_order_relaxed);
            std::uint64_t e = h->seen.load(std::memory_order_acquire);
            for (;;) {
                if (d >= e) {
                    e = h->enq.load(std::memory_order_acquire);
                    if (d >= e) break;
                    // Tell the other consumers, so that they claim up to e
                    // without reading the producer's line. A store that
                    // lands after a larger one only costs a later reader a
                    // reload.
                    h->seen.store(e, std::memory_order_release);
                }
                // A failed claim leaves d at the index another consumer
                // took, and the published count read above still holds.
                if (h->deq.compare_exchange_weak(d, d + 1, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    cell& c = h->cells[d];
                    f(std::move(*c.slot()));
                    c.slot()->~Slot();
                    return true;
                }
            }
            if (e < kSegment) return false;
            // Drained and full: the next segment, if linked, holds what
            // follows; if not, the queue is empty.
            segment* next = h->next.load(std::memory_order_acquire);
            if (!next) return false;
            if (head_.compare_exchange_strong(h, next, std::memory_order_seq_cst))
                hazard::retire(hp, h, &free_segment);
        }
    }

#ifdef WTC_CHECK
    void claim_producer() {
        thread_local const std::uintptr_t me = static_cast<std::uintptr_t>(pthread_self());
        std::uintptr_t seen = producer_.load(std::memory_order_relaxed);
        if (seen == me) return;
        if (seen == 0 && producer_.compare_exchange_strong(seen, me, std::memory_order_relaxed)) return;
        if (seen == me) return;
        std::fprintf(stderr,
                     "wtc: SPMC contract violated: a second producer thread on queue %p "
                     "(first %#lx, now %#lx)\n",
                     static_cast<void*>(this), static_cast<unsigned long>(seen),
                     static_cast<unsigned long>(me));
        std::abort();
    }
    alignas(kLine) std::atomic<std::uintptr_t> producer_{0};
#endif

    // The producer's own: the segment it fills and how many of its cells.
    alignas(kLine) segment* tail_;
    std::uint64_t filled_ = 0;
    alignas(kLine) std::atomic<segment*> head_;
    char pad_[kLine];
};

}  // namespace wtc::rt
