// wtc::rt::segment_queue: an unbounded linearisable FIFO queue for any
// number of producers and consumers, or for any number of producers and one
// consumer, over a list of array segments whose cells are taken with one
// fetch-and-add.
//
// The general-purpose queue the shim falls back to wherever the analysis
// cannot prove a side single, and the queue for many producers and one
// consumer. What the compiler must prove for the single-consumer form:
//
//   Over the object's whole lifetime exactly one thread calls try_pop, any
//   number call push, and the destructor runs after all of them have made
//   their last call (ordered by a join or equivalent).
//
// The multi-consumer form needs nothing but the last clause.
//
// Semantics, the dialect's contract (decisions/0003): every push
// happens-before the pop that returns its element; the queue is
// linearisable and FIFO per producer; a producer's batch comes out in its
// order; no capacity. try_pop never blocks on an empty queue. A
// multi-consumer pop that takes a cell a push has reserved waits for that
// push to fill it, so the pop is not lock-free: its progress is that of a
// lock whose holder is between two stores. The single consumer does not
// wait: it returns false at a reserved cell, even if later pushes have
// finished, and finds the element there when it comes back, as a consumer
// that spins on try_pop, every user of the shim, does a moment later.
//
// Design: the fetch-and-add array queue ("FAAArrayQueue", in "A wait-free
// queue with wait-free memory reclamation", PPoPP 2017 poster, and its
// public implementation). The queue is a list of segments of kSegment
// cells. A push takes the next cell index of the tail segment with a
// fetch-and-add, constructs its element in the cell and marks the cell full;
// a pop takes the next index of the head segment the same way and moves the
// element out. A push whose index falls past the segment's end links a new
// segment holding its element, or moves to the one another push linked; a
// pop past the end moves the head on and retires the segment through hazard
// pointers (hazard.hpp). There is one allocation per segment, none per
// element, and the producers and the consumers each meet on one counter,
// never on a lock.
//
// A pop that reaches a cell no push has reserved yet (its index is at or
// past the segment's push counter) marks it skipped, and the push that later
// takes that index finds it skipped and takes the next one: this keeps a pop
// from waiting on an empty queue. A pop that reaches a cell a push has
// reserved waits for it, so that a batch reserved in one step is never torn
// by a skip in its middle. With one consumer nothing is ever skipped: the
// consumer returns false at an empty cell and comes back to the same one.
//
// A batch takes its cells with one fetch-and-add and fills them in order.
// If a pop skipped one of them in the race described above, the cells after
// it are given up and the rest of the batch is pushed again, so that no
// element of the batch overtakes an earlier one.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <utility>

#include "hazard.hpp"

#ifdef WTC_CHECK
#include <pthread.h>
#endif

namespace wtc::rt {

template <class Slot, bool kManyConsumers = true, unsigned kSegment = 1024, unsigned kLine = 64>
class segment_queue {
    enum : std::uint32_t { kEmpty = 0, kFull = 1, kSkipped = 2 };
    enum : unsigned { kTail = 0, kHead = 1 };
    // How long a pop waits, in pause instructions, for a push to reserve
    // the cell it took before it skips the cell.
    static constexpr unsigned kPatience = 64;

    struct cell {
        std::atomic<std::uint32_t> state{kEmpty};
        alignas(Slot) unsigned char raw[sizeof(Slot)];
        Slot* slot() { return std::launder(reinterpret_cast<Slot*>(raw)); }
    };

    struct segment {
        alignas(kLine) std::atomic<std::uint64_t> deq{0};
        alignas(kLine) std::atomic<std::uint64_t> enq{0};
        alignas(kLine) std::atomic<segment*> next{nullptr};
        alignas(kLine) cell cells[kSegment];
    };

    static void free_segment(void* p) { delete static_cast<segment*>(p); }

    static void pause() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#endif
    }

 public:
    segment_queue() {
        segment* s = new segment;
        head_.store(s, std::memory_order_relaxed);
        tail_.store(s, std::memory_order_relaxed);
    }

    ~segment_queue() {
        segment* s = head_.load(std::memory_order_relaxed);
        std::uint64_t from = s->deq.load(std::memory_order_relaxed);
        while (s) {
            for (std::uint64_t i = from; i < kSegment; ++i) {
                cell& c = s->cells[i];
                if (c.state.load(std::memory_order_relaxed) == kFull) c.slot()->~Slot();
            }
            segment* next = s->next.load(std::memory_order_relaxed);
            delete s;
            s = next;
            from = 0;
        }
    }

    segment_queue(const segment_queue&) = delete;
    segment_queue& operator=(const segment_queue&) = delete;

    template <class S>
    void push(S&& s) {
        Slot one(std::forward<S>(s));
        push_run(&one, 1);
    }

    // Pushes n elements in their order, moving them out of first[0..n).
    void push_run(Slot* first, std::size_t n) {
        hazard::record& hp = hazard::mine();
        std::size_t k = 0;
        while (k < n) {
            segment* t = hazard::protect(hp, kTail, tail_);
            std::uint64_t want = n - k;
            std::uint64_t i = t->enq.fetch_add(want, std::memory_order_relaxed);
            std::uint64_t end = i + want;
            if (i < kSegment) {
                std::uint64_t stop = end < kSegment ? end : kSegment;
                std::uint64_t j = i;
                for (; j < stop; ++j, ++k) {
                    cell& c = t->cells[j];
                    ::new (static_cast<void*>(c.raw)) Slot(std::move(first[k]));
                    if constexpr (kManyConsumers) {
                        std::uint32_t e = kEmpty;
                        if (c.state.compare_exchange_strong(e, kFull, std::memory_order_release,
                                                            std::memory_order_relaxed))
                            continue;
                        // A pop skipped the cell before this push reserved it:
                        // take the element back, give up the cells after it,
                        // and push the rest again.
                        first[k] = std::move(*c.slot());
                        c.slot()->~Slot();
                        for (std::uint64_t r = j + 1; r < stop; ++r) {
                            std::uint32_t z = kEmpty;
                            t->cells[r].state.compare_exchange_strong(z, kSkipped,
                                                                      std::memory_order_relaxed);
                        }
                        break;
                    } else {
                        c.state.store(kFull, std::memory_order_release);
                    }
                }
                if (k == n) break;
                if (end <= kSegment) continue;  // retry from a later cell
            }
            // Past the segment's end: the next segment, linked by whichever
            // push gets there first, carries what is left.
            segment* next = t->next.load(std::memory_order_acquire);
            for (unsigned spin = 0; !next && spin < 64; ++spin) {
                pause();
                next = t->next.load(std::memory_order_acquire);
            }
            if (!next) {
                segment* fresh = new segment;
                std::uint64_t m = 0;
                for (; m < kSegment && k + m < n; ++m) {
                    cell& c = fresh->cells[m];
                    ::new (static_cast<void*>(c.raw)) Slot(std::move(first[k + m]));
                    c.state.store(kFull, std::memory_order_relaxed);
                }
                fresh->enq.store(m, std::memory_order_relaxed);
                segment* none = nullptr;
                if (t->next.compare_exchange_strong(none, fresh, std::memory_order_release,
                                                    std::memory_order_acquire)) {
                    tail_.compare_exchange_strong(t, fresh, std::memory_order_release,
                                                  std::memory_order_relaxed);
                    k += m;
                    continue;
                }
                for (std::uint64_t r = 0; r < m; ++r) {
                    first[k + r] = std::move(*fresh->cells[r].slot());
                    fresh->cells[r].slot()->~Slot();
                }
                delete fresh;
                next = none;
            }
            tail_.compare_exchange_strong(t, next, std::memory_order_release,
                                          std::memory_order_relaxed);
        }
        hazard::clear(hp, kTail);
    }

    bool try_pop(Slot& out) {
#ifdef WTC_CHECK
        if constexpr (!kManyConsumers) claim_consumer();
#endif
        hazard::record& hp = hazard::mine();
        bool got = kManyConsumers ? pop_many(hp, out) : pop_one(hp, out);
        if constexpr (kManyConsumers) hazard::clear(hp, kHead);
        return got;
    }

 private:
    // Moves the head past a drained segment and retires it. The tail is
    // moved past it first, so that no push can find it through tail_ once
    // it is retired. Both exchanges are sequentially consistent, as the
    // readers' publish-and-reload is: a reader whose reload still saw the
    // segment is then ordered before them, and the scan that frees the
    // segment, later in this thread, finds the reader's slot.
    void advance_head(hazard::record& hp, segment* h, segment* next) {
        segment* t = h;
        tail_.compare_exchange_strong(t, next, std::memory_order_seq_cst);
        if constexpr (kManyConsumers) {
            if (!head_.compare_exchange_strong(h, next, std::memory_order_seq_cst)) return;
        } else {
            head_.store(next, std::memory_order_relaxed);
        }
        hazard::retire(hp, h, &free_segment);
    }

    bool pop_many(hazard::record& hp, Slot& out) {
        for (;;) {
            segment* h = hazard::protect(hp, kHead, head_);
            if (h->deq.load(std::memory_order_relaxed) >= h->enq.load(std::memory_order_acquire) &&
                !h->next.load(std::memory_order_acquire))
                return false;
            std::uint64_t i = h->deq.fetch_add(1, std::memory_order_relaxed);
            if (i >= kSegment) {
                segment* next = h->next.load(std::memory_order_acquire);
                if (!next) return false;
                advance_head(hp, h, next);
                continue;
            }
            cell& c = h->cells[i];
            std::uint32_t st = c.state.load(std::memory_order_acquire);
            if (st == kEmpty) {
                // Not reserved yet: give the pushes a moment to reach it
                // before skipping it. Consumers that outnumber the producers
                // otherwise skip every cell they reach early, and each skip
                // is a retry for the push that comes to it.
                for (unsigned spin = 0;
                     spin < kPatience && i >= h->enq.load(std::memory_order_acquire); ++spin)
                    pause();
                if (i < h->enq.load(std::memory_order_acquire)) {
                    // Reserved: its push is between taking the cell and
                    // filling it, or about to give it up.
                    for (unsigned spin = 0; (st = c.state.load(std::memory_order_acquire)) == kEmpty;
                         ++spin) {
                        if (spin < 1024) pause();
                        else std::this_thread::yield();
                    }
                } else if (c.state.compare_exchange_strong(st, kSkipped, std::memory_order_acquire,
                                                           std::memory_order_acquire)) {
                    continue;
                }
            }
            if (st == kSkipped) continue;
            out = std::move(*c.slot());
            c.slot()->~Slot();
            return true;
        }
    }

    bool pop_one(hazard::record& hp, Slot& out) {
        for (;;) {
            segment* h = head_.load(std::memory_order_relaxed);
            std::uint64_t i = h->deq.load(std::memory_order_relaxed);
            if (i >= kSegment) {
                segment* next = h->next.load(std::memory_order_acquire);
                if (!next) return false;
                advance_head(hp, h, next);
                continue;
            }
            cell& c = h->cells[i];
            if (c.state.load(std::memory_order_acquire) != kFull) return false;
            out = std::move(*c.slot());
            c.slot()->~Slot();
            h->deq.store(i + 1, std::memory_order_relaxed);
            return true;
        }
    }

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

    alignas(kLine) std::atomic<segment*> head_;
    alignas(kLine) std::atomic<segment*> tail_;
    char pad_[kLine];
};

}  // namespace wtc::rt
