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
// order; no capacity. try_pop never blocks on an empty queue, and answers
// empty only when every push that finished before it began has been popped.
// A multi-consumer pop that takes a cell a push has reserved waits for that
// push to fill it, so the pop is not lock-free: its progress is that of a
// lock whose holder is between two stores. The single consumer does not
// wait: at a reserved cell it takes a later full one if there is one, and
// the reserved cell's element when it is filled (pop_one).
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
// by a skip in its middle. With one consumer nothing is ever skipped: at a
// cell no push has taken the consumer answers empty and comes back to the
// same cell; at one a push has taken and not filled it looks past it.
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
#include <type_traits>
#include <utility>
#include <vector>

#include "hazard.hpp"

#ifdef WTC_CHECK
#include <pthread.h>
#endif

namespace wtc::rt {

// Points where a test may hold a thread or count steps. A queue calls its
// Hooks type's static members there; no_hooks, the default, does nothing,
// and a test passes its own type, so that its hooks live in another
// instantiation and cost the others nothing.
struct no_hooks {
    // In a push, after it has taken its cells and before it fills them.
    static void reserved() {}
    // In a multi-consumer pop: stage 0 after it has seen the queue
    // non-empty and before it takes a cell, stage 1 after it has taken the
    // cell and before it reads the cell's state.
    static void pop_stage(unsigned) {}
    // In the single consumer's pop, after it has found a full cell past
    // its head (among the holes or by walking on) and before it looks at
    // the holes again.
    static void between_looks() {}
    // In the single consumer's pop, at each cell it looks at past its head.
    static void look() {}
};

template <class Slot, bool kManyConsumers = true, unsigned kSegment = 1024, unsigned kLine = 64,
          class Hooks = no_hooks>
class segment_queue {
    // kSkipped: a multi-consumer pop passed the cell before any push took
    // it. kTaken: the single consumer took the element out of order (see
    // pop_one), and its in-order walk passes the cell when it gets there.
    enum : std::uint32_t { kEmpty = 0, kFull = 1, kSkipped = 2, kTaken = 3 };
    enum : unsigned { kTail = 0, kHead = 1 };
    // How long a pop waits, in pause instructions, for a push to reserve
    // the cell it took before it skips the cell.
    static constexpr unsigned kPatience = 64;
    // The same for the single consumer, while it is taking a stream, before
    // it reads the producers' count to answer empty (pop_one).
    static constexpr unsigned kOnePatience = 4;

    // Raw storage for one element, the size and alignment of the element,
    // so that the cells are an array of elements and a 64-byte element
    // occupies exactly one line.
    struct cell {
        alignas(Slot) unsigned char raw[sizeof(Slot)];
        Slot* slot() { return std::launder(reinterpret_cast<Slot*>(raw)); }
    };

    // The cells' states live in their own array, sixteen to a line, apart
    // from the elements. With the state inside the cell a 64-byte element
    // straddled two lines and shared each with its neighbours' states, so
    // a pop that followed its push closely took four line transfers per
    // element; kept apart, it takes the element's line and a sixteenth of
    // the states' line.
    struct segment {
        alignas(kLine) std::atomic<std::uint64_t> deq{0};
        alignas(kLine) std::atomic<std::uint64_t> enq{0};
        alignas(kLine) std::atomic<segment*> next{nullptr};
        alignas(kLine) std::atomic<std::uint32_t> state[kSegment];
        alignas(kLine) cell cells[kSegment];
        segment() {
            for (unsigned i = 0; i < kSegment; ++i) state[i].store(kEmpty, std::memory_order_relaxed);
        }
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
        if constexpr (!kManyConsumers) one_.holes.reserve(kHolesReserved);
    }

    ~segment_queue() {
        segment* s = head_.load(std::memory_order_relaxed);
        std::uint64_t from = s->deq.load(std::memory_order_relaxed);
        while (s) {
            for (std::uint64_t i = from; i < kSegment; ++i) {
                if (s->state[i].load(std::memory_order_relaxed) == kFull) s->cells[i].slot()->~Slot();
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
            Hooks::reserved();
            std::uint64_t end = i + want;
            if (i < kSegment) {
                std::uint64_t stop = end < kSegment ? end : kSegment;
                std::uint64_t j = i;
                for (; j < stop; ++j, ++k) {
                    cell& c = t->cells[j];
                    ::new (static_cast<void*>(c.raw)) Slot(std::move(first[k]));
                    if constexpr (kManyConsumers) {
                        std::uint32_t e = kEmpty;
                        if (t->state[j].compare_exchange_strong(e, kFull, std::memory_order_release,
                                                                std::memory_order_relaxed))
                            continue;
                        // A pop skipped the cell before this push reserved it:
                        // take the element back, give up the cells after it,
                        // and push the rest again.
                        first[k] = std::move(*c.slot());
                        c.slot()->~Slot();
                        for (std::uint64_t r = j + 1; r < stop; ++r) {
                            std::uint32_t z = kEmpty;
                            t->state[r].compare_exchange_strong(z, kSkipped,
                                                                std::memory_order_relaxed);
                        }
                        break;
                    } else {
                        t->state[j].store(kFull, std::memory_order_release);
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
                    ::new (static_cast<void*>(fresh->cells[m].raw)) Slot(std::move(first[k + m]));
                    fresh->state[m].store(kFull, std::memory_order_relaxed);
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

    // Pops the oldest element into f, which is called once, with the
    // element as an rvalue still in its cell, or not at all when the queue
    // was empty; the cell is released after f returns. A caller that wants
    // the element somewhere without a default-constructed object to assign
    // to, or wants to build something else from it, pops this way.
    template <class F>
    bool consume(F&& f) {
        static_assert(std::is_nothrow_invocable_v<F, Slot&&>,
                      "the taker runs with the element still in its cell, which is released after "
                      "it returns; it must not throw");
#ifdef WTC_CHECK
        if constexpr (!kManyConsumers) claim_consumer();
#endif
        hazard::record& hp = hazard::mine();
        bool got;
        if constexpr (kManyConsumers)
            got = pop_many(hp, f);
        else
            got = pop_one(hp, f);
        if constexpr (kManyConsumers) hazard::clear(hp, kHead);
        return got;
    }

    bool try_pop(Slot& out) {
        return consume([&](Slot&& s) noexcept { out = std::move(s); });
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

    template <class F>
    bool pop_many(hazard::record& hp, F& f) {
        for (;;) {
            segment* h = hazard::protect(hp, kHead, head_);
            if (h->deq.load(std::memory_order_relaxed) >= h->enq.load(std::memory_order_acquire) &&
                !h->next.load(std::memory_order_acquire))
                return false;
            Hooks::pop_stage(0);
            std::uint64_t i = h->deq.fetch_add(1, std::memory_order_relaxed);
            if (i >= kSegment) {
                segment* next = h->next.load(std::memory_order_acquire);
                if (!next) return false;
                advance_head(hp, h, next);
                continue;
            }
            Hooks::pop_stage(1);
            cell& c = h->cells[i];
            std::atomic<std::uint32_t>& state = h->state[i];
            std::uint32_t st = state.load(std::memory_order_acquire);
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
                    for (unsigned spin = 0; (st = state.load(std::memory_order_acquire)) == kEmpty;
                         ++spin) {
                        if (spin < 1024) pause();
                        else std::this_thread::yield();
                    }
                } else if (state.compare_exchange_strong(st, kSkipped, std::memory_order_acquire,
                                                         std::memory_order_acquire)) {
                    continue;
                }
            }
            if (st == kSkipped) continue;
            f(std::move(*c.slot()));
            c.slot()->~Slot();
            return true;
        }
    }

    // The single consumer keeps its distance from the producers, as the
    // spsc ring's does: at the first cell of every pair of lines of states
    // (kSlipMax states) it waits a
    // bounded while for the last cell of its batch to be filled, so that
    // the states' line is read once the pushes are done with it rather
    // than once per push. The batch doubles while the pushes stay two
    // batches ahead and halves when a wait runs out, so a trickle never
    // waits. Multi-consumer pops do not slip: a cell they wait for could
    // be the one another consumer is about to take.
    static constexpr unsigned kSlipMax = 2 * kLine / sizeof(std::uint32_t);
    static constexpr unsigned kSlipPauses = 64;
    static constexpr unsigned kSlipGapMax = 4;
    unsigned batch_ = 1;

    void slip_one(segment* h, std::uint64_t i) {
        if (i % kSlipMax != 0) return;
        std::uint64_t last = i + batch_ - 1;
        if (last >= kSegment) return;
        if (h->state[last].load(std::memory_order_acquire) == kFull) {
            std::uint64_t ahead = i + 2 * batch_ - 1;
            if (batch_ < kSlipMax && ahead < kSegment &&
                h->state[ahead].load(std::memory_order_acquire) == kFull)
                batch_ *= 2;
            return;
        }
        unsigned gap = 1;
        for (unsigned waited = 0; waited < kSlipPauses; gap = gap < kSlipGapMax ? 2 * gap : gap) {
            for (unsigned k = 0; k < gap; ++k) pause();
            waited += gap;
            if (h->state[last].load(std::memory_order_acquire) == kFull) return;
        }
        if (batch_ > 1) batch_ /= 2;
    }

    // The single consumer takes the cells in index order while they are
    // full. At a cell that is not, it may not simply answer empty: a push
    // may have taken that cell and not yet filled it while a later push has
    // finished, and the queue is then not empty, whatever order the
    // unfinished push is given. So it walks on, among the cells pushes have
    // taken, to the first full one, noting every cell it passes as a hole
    // (taken by a push, not yet filled); if it finds none, every push that
    // has finished has been popped and empty is the right answer. If it
    // finds one, it looks at the holes again, from the earliest, until the
    // look finds no earlier full one (a hole may have been filled since it
    // was passed, by a push that finished before the found cell's push
    // began), takes the earliest full cell, and marks it taken; the
    // in-order walk steps over taken cells later. While there are holes,
    // the next pop looks at them first and walks on from where the last
    // walk stopped, so no cell is walked over twice. This is the dequeue of
    // "Jiffy: a fast, memory efficient, wait-free multi-producers
    // single-consumer queue" (arXiv 2010.14189), with the holes kept in a
    // list. It never waits. A pop looks at each of the h holes up to h
    // times while the look keeps finding earlier ones, so O(h^2), and
    // otherwise costs O(1) amortised: the walk ahead and the step over
    // taken cells each pass a cell once, and a segment's count is read
    // once per segment walked into. h counts cells, filled or not, that
    // the consumer has passed and not yet taken: one batch still being
    // filled leaves up to its length of them, so a poll behind it costs
    // O(batch). Past kHolesReserved holes the list allocates. A push that
    // never fills its cell keeps the head where it is for ever, so the
    // segments after it are never freed. Only this thread retires
    // segments, so the segments it reads ahead need no hazard pointer.
    template <class F>
    bool pop_one(hazard::record& hp, F& f) {
        for (;;) {
            segment* h = head_.load(std::memory_order_relaxed);
            std::uint64_t i = h->deq.load(std::memory_order_relaxed);
            if (i >= kSegment) {
                segment* next = h->next.load(std::memory_order_acquire);
                if (!next) return false;
                advance_head(hp, h, next);
                continue;
            }
            std::uint32_t st = h->state[i].load(std::memory_order_acquire);
            if (st == kTaken) {
                h->deq.store(i + 1, std::memory_order_relaxed);
                continue;
            }
            if (one_.holes.empty()) {
                if (st == kFull) {
                    slip_one(h, i);
                    take(h, i, f);
                    h->deq.store(i + 1, std::memory_order_relaxed);
                    one_.streaming = true;
                    return true;
                }
                // While a stream is being taken, the next cell is most
                // likely about to be filled: give it a few pauses on this
                // cell's line before reading the count, the producers'
                // line, which every push writes. An idle poll does not wait.
                if (one_.streaming) {
                    for (unsigned spin = 0; spin < kOnePatience && st == kEmpty; ++spin) {
                        pause();
                        st = h->state[i].load(std::memory_order_acquire);
                    }
                    if (st == kFull) continue;
                }
                one_.walk = {h, i};
            }
            one_.streaming = pop_ahead(f);
            return one_.streaming;
        }
    }

    struct position {
        segment* seg = nullptr;
        std::uint64_t index = 0;
    };

    bool full(position p) {
        Hooks::look();
        return p.seg->state[p.index].load(std::memory_order_acquire) == kFull;
    }

    // Pops the earliest full cell at or past the head, as pop_one says, or
    // answers empty.
    template <class F>
    bool pop_ahead(F& f) {
        std::size_t k = 0;
        while (k < one_.holes.size() && !full(one_.holes[k])) ++k;
        if (k == one_.holes.size()) {
            if (!walk_on()) return false;
            k = one_.holes.size() - 1;  // the full cell the walk stopped at
        }
        Hooks::between_looks();
        for (;;) {
            std::size_t m = 0;
            while (m < k && !full(one_.holes[m])) ++m;
            if (m == k) break;
            k = m;
        }
        position c = one_.holes[k];
        one_.holes.erase(one_.holes.begin() + static_cast<std::ptrdiff_t>(k));
        take(c.seg, c.index, f);
        c.seg->state[c.index].store(kTaken, std::memory_order_relaxed);
        return true;
    }

    // Walks on from where the last walk stopped, among the cells pushes
    // have taken, adding each to the holes, up to and including the first
    // full one; false if there is none. A push past a segment's end takes
    // an index there too, so a segment whose count is past its end is
    // followed into the next one.
    bool walk_on() {
        segment* s = one_.walk.seg;
        std::uint64_t j = one_.walk.index;
        for (;;) {
            std::uint64_t taken = s->enq.load(std::memory_order_acquire);
            std::uint64_t end = taken < kSegment ? taken : kSegment;
            for (; j < end; ++j) {
                one_.holes.push_back({s, j});
                if (full({s, j})) {
                    one_.walk = {s, j + 1};
                    return true;
                }
            }
            one_.walk = {s, j};
            if (taken <= kSegment) return false;
            segment* next = s->next.load(std::memory_order_acquire);
            if (!next) return false;
            s = next;
            j = 0;
            one_.walk = {s, 0};
        }
    }

    template <class F>
    static void take(segment* s, std::uint64_t i, F& f) {
        cell& c = s->cells[i];
        f(std::move(*c.slot()));
        c.slot()->~Slot();
    }

    // The single consumer's own state: the holes, in index order (the last
    // may be the full cell a walk stopped at), where the next walk starts,
    // and whether the last pop took an element. Room for this many holes
    // is made when the queue is built; more allocate. The multi-consumer
    // form carries none of it.
    static constexpr std::size_t kHolesReserved = 64;
    struct one_state {
        std::vector<position> holes;
        position walk;
        bool streaming = false;
    };
    struct no_state {};
    [[no_unique_address]] std::conditional_t<kManyConsumers, no_state, one_state> one_;

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
