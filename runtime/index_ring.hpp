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
// cells are counted from it too. Over the wait-free ring the fact is
// stronger: also at most `threads` threads alive at once that have
// operated on the queue (wcq_ring's thread slots). Built with WTC_CHECK,
// the queue counts the operations in flight and aborts when they are more
// than `threads`.
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
//           step: it takes its element when it dequeues its index (a
//           slow-path dequeue of the wait-free ring, when its request is
//           finished, by it or by a helper) and decrements the count after
//           its ring operation returns, so a place also stays reserved for
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
// enqueue that ignores the safe bit; 3, a slow-path enqueue that takes a
// dequeue's kBot at its round for its own entry. Nothing otherwise.
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

#ifndef WTC_RT_MODEL
#include "hazard.hpp"
#endif

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
    // The wait-free ring's: every iteration of every loop of an operation
    // (its own steps), a slow-path round started, and a helping visit to
    // another thread's pending request.
    static void step() {}
    static void slow_round() {}
    static void helping() {}
    // In a slow-path enqueue, by the requester or a helper, after the
    // entry is produced and before the threshold is reset.
    static void produced() {}
};

// Thread slots: which of a wait-free ring's per-thread states a thread
// uses. Each ring has its own `threads` slots; a thread takes one at its
// first operation on the ring and holds it until it exits, when the slot
// becomes free again without the thread doing anything: a slot names its
// holder by its hazard record's token (hazard.hpp: a 64-bit name no other
// holder of any record ever has) and the record's index, and a token its
// record no longer shows names a thread that has exited, or one that did
// not survive a fork. Only a thread whose record is tied to its exit takes
// a slot: one whose record is untied (hazard.hpp: its exit could not be
// hooked), and an operation from a thread whose exit has already given its
// record back (one from a later key destructor, which borrows a record
// with no token for its own length), would hold it with a token nothing
// ends. Those run the fallback below, counted.
//
// Taking one. The thread looks for the slot it holds (its cached index
// first), and otherwise takes the first slot, in index order, that is free
// or names a thread that has exited, with one compare-and-swap each; it
// tells a live holder from a gone one by one read of the holder's record.
// At most `threads` slots, so wait-free. With at most `threads` threads
// alive at once that have operated on the ring, it always finds one. A
// thread is present from its first look until its exit gives its record
// back (operations after that take no slot), and the slots in index
// order are a renaming by test-and-set: call a present thread's rank the
// index of the slot it holds, or, while it looks, the number of slots it
// has found held; it never decreases while the thread is present. A
// thread finds a slot held at an instant inside its look at it: when it
// read a token that a record still showed, or, when its compare-and-swap
// failed, when the other thread's compare-and-swap took the slot, which
// came between this thread's read and its own and at which that thread was
// present (it may have exited since; the argument needs only that
// instant). By
// induction on b, at most `threads` - b present threads have rank b or
// more at any instant. The number with rank b + 1 or more grows only when
// a thread passes from rank b to b + 1, by finding slot b held by a
// present thread, whose rank is b; if just after that pass `threads` - b
// threads had rank b + 1 or more (the passing one included), counting the
// holder there would be `threads` - b + 1 of rank b or more, against the
// induction. For b = `threads`: no thread ever finds every slot held.
//
// More threads than that. The thread that finds no slot runs the
// operation without a state: SCQ's loops, without patience or helping,
// lock-free; it looks again at its next operation. Every operation stays
// correct, and fallbacks() counts these; but such a thread's
// fetch-and-adds are unbounded and it helps nobody, so while one exists
// the others' operations are not wait-free either. Built with WTC_CHECK,
// the ring refuses it instead: it reports the thread and aborts.
//
// The model-checking build (WTC_RT_MODEL) supplies its own self().
#ifndef WTC_RT_MODEL
class thread_slots {
public:
    explicit thread_slots(unsigned n)
        : n_(n), owner_(static_cast<owner*>(aligned_alloc(alignof(owner), sizeof(owner) * (n ? n : 1)))) {
        if (!owner_) abort();
        for (unsigned i = 0; i < n_; ++i) owner_[i] = owner{0, 0};
        // Set the domain up here, as the other hazard users' constructors
        // do, so that no operation takes prepare()'s static guard,
        // allocates the first chunk or creates the key.
        hazard::prepare();
    }
    ~thread_slots() { free(owner_); }
    thread_slots(const thread_slots&) = delete;
    thread_slots& operator=(const thread_slots&) = delete;

    // The calling thread's slot, taken at its first call; n if it takes
    // none. took_over is set if the slot was taken from a thread that had
    // exited, which may have left a request pending (a fork's child).
    unsigned self(bool& took_over) {
        const hazard::record* r = hazard::tied_record();
        if (!r) return n_;  // see the top: no slot while untied or exiting
        const std::uint64_t me = hazard::token(*r);
        cached& c = t_cache[(reinterpret_cast<std::uintptr_t>(this) >> 6) % kCache];
        if (c.slots == this && c.slot < n_ && token_of(owner_[c.slot]) == me) return c.slot;
        return find(me, r->id, c, took_over);
    }

private:
    // A slot's holder: its token, 0 for none, and its record's index, set
    // together by a double-width compare-and-swap, which the wait-free
    // ring needs anyway; each half is read alone with a 64-bit load, and a
    // pair read across a change only fails the compare-and-swap.
    struct alignas(16) owner {
        std::uint64_t token;
        std::uint64_t id;
    };
    static std::uint64_t token_of(owner& o) { return __atomic_load_n(&o.token, __ATOMIC_ACQUIRE); }
    static std::uint64_t id_of(owner& o) { return __atomic_load_n(&o.id, __ATOMIC_ACQUIRE); }
    static bool take(owner& o, std::uint64_t token, std::uint64_t id, std::uint64_t me, std::uint64_t my_id) {
        using u128 = unsigned __int128;
        const u128 e = u128(id) << 64 | token;
        return __sync_bool_compare_and_swap(reinterpret_cast<u128*>(&o), e, u128(my_id) << 64 | me);
    }

    // The thread's recent rings and its slot in each: a hint, checked
    // against the slot's owner before it is used, so a ring built later at
    // the same address does not mislead it.
    static constexpr unsigned kCache = 8;
    struct cached {
        const thread_slots* slots;
        unsigned slot;
    };
    static inline thread_local cached t_cache[kCache] = {};

    [[gnu::noinline]] unsigned find(std::uint64_t me, unsigned my_id, cached& c, bool& took_over) {
        for (unsigned i = 0; i < n_; ++i)
            if (token_of(owner_[i]) == me) {
                c = {this, i};
                return i;
            }
        for (unsigned i = 0; i < n_; ++i) {
            const std::uint64_t o = token_of(owner_[i]);
            const std::uint64_t id = id_of(owner_[i]);
            if (o != 0 && hazard::held(o, static_cast<unsigned>(id))) continue;
            // Free, or its holder has exited: a failed compare-and-swap
            // means another thread took it meanwhile (see the top).
            if (take(owner_[i], o, id, me, my_id)) {
                c = {this, i};
                took_over = o != 0;
                return i;
            }
        }
#ifdef WTC_CHECK
        fprintf(stderr,
                "wtc: wait-free index ring %p: every one of its %u thread slots is held by a live thread; "
                "more threads than it was built for\n",
                static_cast<const void*>(this), n_);
        abort();
#endif
        return n_;
    }

    const unsigned n_;
    owner* const owner_;
};
#else
class thread_slots {
public:
    explicit thread_slots(unsigned) {}
    unsigned self(bool& took_over);
};
#endif

template <class Hooks = no_ring_hooks>
class scq_ring {
public:
    using u64 = std::uint64_t;

    // A ring for indices below 2^order (order >= 1), holding the first
    // `full` of them (0, 1, ...) when built, or none.
    scq_ring(unsigned order, std::uint32_t full, unsigned threads = 0)
        : order_(order),
          bits_(order + 1),
          size_(u64{1} << (order + 1)),
          entries_(static_cast<std::atomic<u64>*>(malloc(sizeof(std::atomic<u64>) * size_))) {
        (void)threads;  // the wait-free ring's; SCQ keeps no per-thread state
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

// ---------------------------------------------------------------------------
// wcq_ring: the same ring of indices with wait-free operations ("wCQ: a fast
// wait-free queue with bounded memory usage", SPAA 2022).
//
// The fast path is SCQ's, tried kPatienceEnq (kPatienceDeq) times. An
// operation that has not finished by then asks for help: it publishes a
// request in its thread's state (the last position it tried, the index to
// enqueue) and runs the slow path, and every other thread, once every
// kDelay operations, looks at one other thread's state and, if a request
// is pending there, runs the same slow path for it. The slow path takes
// positions with slow_inc rather than a fetch-and-add, so that the helpee
// and its helpers move the global counter once per attempt between them
// (a local counter in the request, set with an INC flag first, then the
// global one, then the flag cleared, with a phase-2 record for the threads
// that find the flag set); an enqueue writes its entry first with the Enq
// bit clear and then sets FIN in the request, after which any thread
// flips Enq, so that only one of the cooperating threads inserts; a
// dequeue sets FIN when it has found its entry or the ring empty. Each
// entry carries a Note, the cycle of the last slow-path operation that
// skipped it, so that helpers which come later skip it too. Entries and
// the global counters are pairs updated with a double-width
// compare-and-swap (CAS2).
//
// The encoding is the authors' published one (the lfqueue repository's
// wfring_cas2.h, dual 2-clause BSD and MIT): counters step by 4, so that a
// request's local counter carries FIN (bit 0) and INC (bit 1); an entry
// holds its index in the low log2(N) bits, Enq at N, IsSafe at 2N and the
// cycle above, with kBot = N - 2 and kBotC = N - 1.
//
// Where this differs from the paper's figures and from that code. The
// first three follow the figures where the code differs; the rest are
// faults that the ring churn of runtime/test/wcq_test (an index lost or
// doubled a few times in a thousand rounds of six threads on the slow-path
// build) and index_queue_test's stress and linearisability rounds found,
// in the figures and the code alike, each fixed where the comment at the
// place says:
//
//   - slow_inc uses the phase-2 record of the thread running it, not the
//     helpee's, so that two helpers never write one record at once; it
//     moves the global counter by one position (4), as the fast path's
//     fetch-and-add does; help() visits every other state in turn.
//   - At a round's position an enqueue that finds the round's own cycle
//     goes on to the next round if a dequeue left kBot there, and stops
//     only if the request produced it.
//   - lookup() passes over a state whose counter matches the position only
//     under its flags, rather than stopping at it, and is given the
//     position without the dequeue's own FIN.
//   - Idle states' counters hold no position, so lookup() cannot take one
//     for the request that produced at position 0.
//   - A phase-2 record is helped only if, read after it, the counter still
//     carries its thread and the record's sequence number is unchanged.
//   - Each state has one phase-2 record per counter.
//   - A slow-path enqueue resets the threshold itself before it returns.
//
// These change no step of the paper's progress argument (each is a check
// on a value already read, or a bounded loop over the states), but the
// paper's proof is of its own algorithm, not of this one.

// Thread states. Each ring has `threads` states, one per slot of its
// thread_slots: a thread uses the state of the slot it holds, from its
// first operation on the ring until it exits, and a later thread takes the
// slot over. A state changes hands only between operations, so to the
// algorithm a state taken over is one thread's state between two of its
// operations; the slot's compare-and-swap, and the end of its old holder's
// token, order everything the old holder did before everything the new
// one does. With more threads alive than `threads`,
// a thread may find no slot (see thread_slots for what it does then).
//
// Progress. Lemmas 5.1-5.5 and Theorem 5.10 of the paper prove every
// operation finishes in finitely many of its own steps; they give no
// closed-form bound, and none is claimed here: wait-free, the bound
// finite and not instantiated. runtime/test/wcq_test measures the steps
// of the operations that complete while another thread is frozen at each
// point of its own (seven threads, a queue of four), and prints the most;
// runtime/README.md records what it measured.
// Wait-freedom also needs the hardware's fetch-and-add, OR and CAS2 to be
// wait-free: x86-64 with cmpxchg16b, AArch64 with LSE (casp), which
// kCas2WaitFree says at compile time and cas2_supported() checks at run
// time. And it needs every thread operating on the ring to hold a slot:
// at most `threads` threads alive at once that have operated on it.
// A thread's first operation on a ring also takes its hazard record, if it
// has none yet, and its slot: bounded, and allocation-free unless
// hazard.hpp's conditions say otherwise (more than kChunk records held at
// once, or a pthread key past glibc's first 32).

// Whether this build's double-width compare-and-swap is one instruction:
// cmpxchg16b on x86-64 (-mcx16), casp on AArch64 (LSE, -march=armv8.1-a
// or later). Elsewhere, or without those flags, it is a loop or a lock.
#if (defined(__x86_64__) && defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)) || \
    (defined(__aarch64__) && defined(__ARM_FEATURE_ATOMICS))
constexpr bool kCas2WaitFree = true;
#else
constexpr bool kCas2WaitFree = false;
#endif

// Whether the processor this runs on has the instruction the build uses.
inline bool cas2_supported() {
#if defined(__x86_64__) && !defined(WTC_RT_MODEL)
    unsigned a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return kCas2WaitFree && (c >> 13 & 1);  // CPUID.1:ECX.CX16
#else
    return kCas2WaitFree;
#endif
}

// The pairs of 64-bit words, by CAS2: lo is an entry's value or a
// counter, hi an entry's Note or the index + 1 of the thread whose
// phase-2 record a counter carries (0: none). The halves are also read
// and updated alone, with 64-bit atomics, as the published code does.
struct wide_words {
    using word = std::uint64_t;
    using sword = std::int64_t;
    static constexpr bool kNeedsCas2 = true;
    struct alignas(16) pair {
        word lo;
        word hi;
    };
    using u128 = unsigned __int128;
    static u128 join(word lo, word hi) { return u128(hi) << 64 | lo; }
    static void init(pair& p, word lo, word hi) {
        p.lo = lo;
        p.hi = hi;
    }
    static word lo(pair& p) { return __atomic_load_n(&p.lo, __ATOMIC_SEQ_CST); }
    static bool cas_lo(pair& p, word& expected, word desired) {
        return __atomic_compare_exchange_n(&p.lo, &expected, desired, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST);
    }
    static word faa_lo(pair& p, word v) { return __atomic_fetch_add(&p.lo, v, __ATOMIC_SEQ_CST); }
    static void or_lo(pair& p, word v) { __atomic_fetch_or(&p.lo, v, __ATOMIC_SEQ_CST); }
    // Both halves at one instant: a CAS2 that writes what it read.
    static void load(pair& p, word& lo, word& hi) {
        const u128 v = __sync_val_compare_and_swap(reinterpret_cast<u128*>(&p), u128(0), u128(0));
        lo = static_cast<word>(v);
        hi = static_cast<word>(v >> 64);
    }
    // CAS2; on failure lo and hi hold what the pair held.
    static bool cas(pair& p, word& lo, word& hi, word new_lo, word new_hi) {
        const u128 e = join(lo, hi);
        const u128 v = __sync_val_compare_and_swap(reinterpret_cast<u128*>(&p), e, join(new_lo, new_hi));
        if (v == e) return true;
        lo = static_cast<word>(v);
        hi = static_cast<word>(v >> 64);
        return false;
    }
};

template <class Hooks = no_ring_hooks, class Words = wide_words, class Ids = thread_slots,
          unsigned kPatienceEnq = 16, unsigned kPatienceDeq = 64, unsigned kDelay = 16>
class wcq_ring {
public:
    using word = typename Words::word;
    using sword = typename Words::sword;
    using pair = typename Words::pair;

    wcq_ring(unsigned order, std::uint32_t full, unsigned threads)
        : order_(order),
          n_(word{1} << (order + 1)),
          nstates_(threads ? threads : 1),
          entries_(static_cast<pair*>(aligned_alloc(alignof(pair), sizeof(pair) * n_))),
          states_(static_cast<state*>(aligned_alloc(64, round64(sizeof(state) * nstates_)))),
          slots_(nstates_) {
        if (order < 1 || order > 30 || full > (word{1} << order) || !entries_ || !states_) {
            fprintf(stderr, "wtc: wait-free index ring of order %u with %u indices\n", order, full);
            abort();
        }
        if (Words::kNeedsCas2 && !cas2_supported()) {
            fprintf(stderr, "wtc: the wait-free index ring needs a double-width compare-and-swap "
                            "this processor or build lacks (cmpxchg16b with -mcx16, or LSE)\n");
            abort();
        }
        const word half = word{1} << order;
        for (word i = 0; i < n_; ++i) Words::init(entries_[i], ~word{0}, word(0) - n_ - 1);
        for (word i = 0; i < full; ++i) Words::init(entries_[map(i)], 2 * n_ + n_ + i, ~word{0});
        (void)half;
        Words::init(head_, 0, 0);
        Words::init(tail_, word(full) << 2, 0);
        for (unsigned i = 0; i < nstates_; ++i) ::new (&states_[i]) state(i, nstates_);
        threshold_.store(full ? threshold_max() : -1, std::memory_order_seq_cst);
    }

    ~wcq_ring() {
        for (unsigned i = 0; i < nstates_; ++i) states_[i].~state();
        free(states_);
        free(entries_);
    }

    wcq_ring(const wcq_ring&) = delete;
    wcq_ring& operator=(const wcq_ring&) = delete;

    std::uint32_t indices() const { return static_cast<std::uint32_t>(n_ / 2); }

    // Operations by threads that found no slot (see thread_slots).
    std::uint64_t fallbacks() const { return fallbacks_.load(std::memory_order_relaxed); }
    // Operations that took the slow path; a hint for tests.
    std::uint64_t slow_paths() const { return slow_.load(std::memory_order_relaxed); }

    void enqueue(std::uint32_t index) {
        bool took_over = false;
        const unsigned me = slots_.self(took_over);
        if (me >= nstates_) return enqueue_fallback(index);
        state& st = states_[me];
        if (took_over) adopt(st, me);
        word eidx = word(index) ^ (n_ - 1);
        if (--st.next_check == 0) help(st, me);
        word tail;
        if (kPatienceEnq == 0) {
            // The test build in which every enqueue takes the slow path: a
            // position taken and left, as a failed try leaves it, so that
            // the request starts from a position no earlier request of
            // this state has had (its local counter never repeats a value).
            tail = Words::faa_lo(tail_, 4);
            Hooks::enq_reserved();
        } else {
            for (unsigned patience = kPatienceEnq;;) {
                Hooks::step();
                tail = Words::faa_lo(tail_, 4);
                Hooks::enq_reserved();
                if (try_enq(tail, eidx)) return;
                if (--patience == 0) break;
            }
        }
        enqueue_slow(eidx, tail, st, me);
    }

    bool dequeue(std::uint32_t& index) {
        if (threshold_.load(std::memory_order_seq_cst) < 0) return false;
        bool took_over = false;
        const unsigned me = slots_.self(took_over);
        if (me >= nstates_) return dequeue_fallback(index);
        state& st = states_[me];
        if (took_over) adopt(st, me);
        if (--st.next_check == 0) help(st, me);
        // A dequeue must try the position it takes: an element may be
        // there, and no other dequeue will take it.
        static_assert(kPatienceDeq >= 1, "a dequeue tries at least once");
        word head = 0;
        for (unsigned patience = kPatienceDeq; patience != 0; --patience) {
            Hooks::step();
            head = Words::faa_lo(head_, 4);
            Hooks::deq_reserved();
            switch (try_deq(head, index, st)) {
            case kOk: return true;
            case kEmpty: return false;
            default: break;
            }
        }
        return dequeue_slow(head, index, st, me);
    }

private:
    enum result { kOk, kEmpty, kRetry };
    static constexpr word kFin = 1, kInc = 2;
    static constexpr word kTerm = 0, kDeq = 1;  // a request's eidx: none, a dequeue

    struct phase2rec {
        std::atomic<word> seq1{1};
        std::atomic<std::atomic<word>*> local{nullptr};
        std::atomic<word> cnt{0};
        std::atomic<word> seq2{0};
    };
    struct alignas(64) state {
        state(unsigned self, unsigned count) : curr((self + 1) % count) {}
        // The owner's alone.
        unsigned next_check = kDelay;
        unsigned curr;  // the next state help() looks at
        // One record per counter. The paper's thread record has one: a
        // thread that left its pointer on one counter (its clearing failed
        // to a fetch-and-add) and prepared the record for a round of the
        // other counter let a reader of the first counter, which still
        // carried the pointer, end that other round's phase 1 (found by
        // runtime/test/wcq_test's ring checks).
        phase2rec phase2_tail;
        phase2rec phase2_head;
        // The request, read by helpers. The local counters start as no
        // position at all (every bit set, FIN and INC included), so that
        // lookup() cannot take an idle state for the request that
        // produced at position 0.
        std::atomic<word> seq1{1};
        std::atomic<word> tail{~word{0}};
        std::atomic<word> init_tail{0};
        std::atomic<word> head{~word{0}};
        std::atomic<word> init_head{0};
        std::atomic<word> eidx{kTerm};
        std::atomic<word> seq2{0};
    };

    static std::size_t round64(std::size_t b) { return (b + 63) / 64 * 64; }
    static bool lt(word x, word y) { return static_cast<sword>(x - y) < 0; }
    static bool le(word x, word y) { return static_cast<sword>(x - y) <= 0; }
    sword threshold_max() const { return static_cast<sword>(n_ / 2 + n_ - 1); }

    // Positions to entries, four 16-byte entries to a line.
    word map(word position) const {
        const word p = position & (n_ - 1);
        const unsigned o = order_ + 1;
        constexpr unsigned kMin = 2;
        if (o <= kMin) return p;
        return (p >> (o - kMin)) | ((p << kMin) & (n_ - 1));
    }

    void reset_threshold() {
        if (threshold_.load(std::memory_order_seq_cst) != threshold_max())
            threshold_.store(threshold_max(), std::memory_order_seq_cst);
    }

    // SCQ's try_enq on the entry half of the pair.
    bool try_enq(word tail, word eidx) {
        const word tcycle = tail | (4 * n_ - 1);
        pair& slot = entries_[map(tail >> 2)];
        word entry = Words::lo(slot);
        for (;;) {
            Hooks::step();
            const word ecycle = entry | (4 * n_ - 1);
            if (lt(ecycle, tcycle) &&
                ((entry | 1) == ecycle ||
                 ((entry | 1) == (ecycle ^ (2 * n_)) && le(Words::lo(head_), tail)))) {
                if (!Words::cas_lo(slot, entry, tcycle ^ eidx)) continue;
                reset_threshold();
                return true;
            }
            return false;
        }
    }

    result try_deq(word head, std::uint32_t& index, state& st) {
        const word hcycle = head | (4 * n_ - 1);
        pair& slot = entries_[map(head >> 2)];
        word entry = Words::lo(slot);
        for (;;) {
            Hooks::step();
            const word ecycle = entry | (4 * n_ - 1);
            if (ecycle == hcycle) {
                // An entry a slow-path enqueue wrote and has not finalised:
                // finalise its request before taking it.
                if (!(entry & n_)) lookup(st, head);
                Words::or_lo(slot, 2 * n_ - 1);
                index = static_cast<std::uint32_t>(entry & (n_ - 1));
                return kOk;
            }
            word next;
            if ((entry | (2 * n_) | 1) != ecycle) {
                next = entry & ~(2 * n_);  // occupied by an older cycle: unsafe
                if (entry == next) break;
            } else {
                next = hcycle ^ ((~entry) & (2 * n_)) ^ 1;  // empty: this cycle, kBot
            }
            if (!lt(ecycle, hcycle)) break;
            if (Words::cas_lo(slot, entry, next)) break;
        }
        const word t = Words::lo(tail_);
        if (le(t, head + 4)) {
            catchup(t, head + 4);
            threshold_.fetch_sub(1, std::memory_order_seq_cst);
            return kEmpty;
        }
        if (threshold_.fetch_sub(1, std::memory_order_seq_cst) <= 0) return kEmpty;
        return kRetry;
    }

    void catchup(word tail, word head) {
        // Bounded, as the paper bounds it: catching up only saves later
        // enqueues positions every dequeue has passed.
        for (int k = 0; k < 64; ++k) {
            Hooks::step();
            word t = tail;
            if (Words::cas_lo(tail_, t, head)) return;
            head = Words::lo(head_);
            tail = Words::lo(tail_);
            if (!lt(tail, head)) return;
        }
    }

    // Sets FIN in the request of the thread whose slow-path enqueue
    // produced the entry at position `tail` (Enq still clear), before the
    // entry is consumed: FIN is what keeps that request's helpers from
    // starting another round once the entry is gone and the slot reused,
    // which would produce the index twice (finalize_request, Figure 5).
    //
    // The producing request's local counter is exactly `tail`, or already
    // has FIN. Another request's may hold `tail` with INC for a moment (a
    // phase 1 that lost the position to this one), so a state whose
    // counter matches only under the flags is passed over, not taken for
    // the one: the paper's and the published code's loop stops at the
    // first match, and when that match was such a state the entry was
    // consumed without FIN (found by runtime/test/wcq_test's ring checks,
    // about once in a thousand rounds on the slow-path build, which runs
    // ten thousand for it).
    void lookup(state& st, word tail) {
        const unsigned me = static_cast<unsigned>(&st - states_);
        for (unsigned k = 0; k < nstates_; ++k) {
            Hooks::step();
            state& other = states_[(me + k) % nstates_];
            word t = tail;
            if (other.tail.compare_exchange_strong(t, tail | kFin, std::memory_order_seq_cst)) return;
            if (t == (tail | kFin)) return;
        }
    }

    // Helps one other thread's pending request, if any, every kDelay
    // operations; the next call looks at the next state (help_threads,
    // Figure 6).
    void help(state& st, unsigned me) {
        st.next_check = kDelay;
        const unsigned i = st.curr;
        st.curr = (st.curr + 1) % nstates_;
        if (st.curr == me) st.curr = (st.curr + 1) % nstates_;
        if (i == me) return;
        state& other = states_[i];
        if (other.eidx.load(std::memory_order_seq_cst) == kTerm) return;
        Hooks::helping();
        const word seq = other.seq2.load(std::memory_order_seq_cst);
        const word eidx = other.eidx.load(std::memory_order_seq_cst);
        const word tail = other.init_tail.load(std::memory_order_seq_cst);
        const word head = other.init_head.load(std::memory_order_seq_cst);
        if (other.seq1.load(std::memory_order_seq_cst) != seq) return;  // not that request any more
        if (eidx > kDeq) do_enqueue_slow(eidx, seq, tail, other, me);
        else if (eidx == kDeq) do_dequeue_slow(seq, head, other, me);
    }

    phase2rec& record_of(state& st, const pair& global) {
        return &global == &tail_ ? st.phase2_tail : st.phase2_head;
    }

    // Completes phase 2 of whichever request the global counter carries,
    // clears it, and returns the counter; false if `mine` has FIN, which
    // ends the caller's slow path (load_global_help_phase2, Figure 7).
    //
    // One check the paper and the published code do not make: the record
    // is acted on only if, read after it, the counter still carries its
    // thread. A phase-2 record is its thread's, and the thread reuses it
    // for its next slow_inc once it has cleared the counter's pointer. A
    // thread that loaded the pointer before that clearing and reads the
    // record after the reuse would otherwise end the next round's phase 1
    // although that round never moved the global counter; the round's
    // position, handed out by a fetch-and-add meanwhile, then belonged to
    // two operations (found by runtime/test/wcq_test's ring checks: one
    // element consumed by two dequeues). The pointer is cleared before
    // the record is reused, and only its thread sets it again, after
    // preparing the record, so a counter that carries the thread after the
    // read, with the record's sequence number unchanged after that, carries
    // this very record.
    bool load_global_help_phase2(pair& global, word& lo, word& hi, std::atomic<word>& mine) {
        for (;;) {
            Hooks::step();
            if (mine.load(std::memory_order_seq_cst) & kFin) return false;
            if (hi == 0) return true;
            phase2rec& ph = record_of(states_[hi - 1], global);
            const word seq = ph.seq2.load(std::memory_order_seq_cst);
            std::atomic<word>* local = ph.local.load(std::memory_order_seq_cst);
            const word cnt = ph.cnt.load(std::memory_order_seq_cst);
            if (ph.seq1.load(std::memory_order_seq_cst) == seq && local) {
                word lo2, hi2;
                Words::load(global, lo2, hi2);
                // And the record unchanged since: its thread may have
                // prepared another and posted that one in between.
                if (hi2 == hi && lt(cnt, lo2) && ph.seq1.load(std::memory_order_seq_cst) == seq) {
                    word ci = cnt + kInc;
                    local->compare_exchange_strong(ci, cnt, std::memory_order_seq_cst);
                }
            }
            if (Words::cas(global, lo, hi, lo, 0)) {
                hi = 0;
                return true;
            }
            // lo and hi reloaded: help again, or find it cleared.
        }
    }

    // slow_F&A (Figure 7): moves the global counter once for the helpee
    // and its helpers together, and leaves `prev` the position this round
    // tries; false once the request has FIN.
    bool slow_inc(pair& global, std::atomic<word>& local, word& prev, bool thld, unsigned me) {
        phase2rec& ph = record_of(states_[me], global);
        word lo, hi;
        Words::load(global, lo, hi);
        word cnt;
        for (;;) {
            Hooks::step();
            if (!load_global_help_phase2(global, lo, hi, local)) return false;
            cnt = lo;
            // Phase 1: the request's local counter to the global one, INC set.
            if (!local.compare_exchange_strong(prev, cnt + kInc, std::memory_order_seq_cst)) {
                if (prev & kFin) return false;
                if (!(prev & kInc)) return true;  // another thread finished this round
                cnt = prev - kInc;                // help the round in progress
            } else {
                prev = cnt + kInc;
            }
            const word seq = ph.seq1.load(std::memory_order_seq_cst) + 1;
            ph.seq1.store(seq, std::memory_order_seq_cst);
            ph.local.store(&local, std::memory_order_seq_cst);
            ph.cnt.store(cnt, std::memory_order_seq_cst);
            ph.seq2.store(seq, std::memory_order_seq_cst);
            lo = cnt;
            hi = 0;
            if (Words::cas(global, lo, hi, cnt + 4, me + 1)) break;
            // lo and hi reloaded.
        }
        if (thld) threshold_.fetch_sub(1, std::memory_order_seq_cst);
        // Phase 2: INC cleared, and the record taken off the counter.
        word ci = cnt + kInc;
        local.compare_exchange_strong(ci, cnt, std::memory_order_seq_cst);
        lo = cnt + 4;
        hi = me + 1;
        Words::cas(global, lo, hi, cnt + 4, 0);
        prev = cnt;
        return true;
    }

    void enqueue_slow(word eidx, word tail, state& st, unsigned me) {
        slow_.fetch_add(1, std::memory_order_relaxed);
        const word seq = st.seq1.load(std::memory_order_seq_cst);
        st.tail.store(tail, std::memory_order_seq_cst);
        st.init_tail.store(tail, std::memory_order_seq_cst);
        st.eidx.store(eidx, std::memory_order_seq_cst);
        st.seq2.store(seq, std::memory_order_seq_cst);
        do_enqueue_slow(eidx, seq, tail, st, me);
        // The entry may have been produced by a helper that has not yet
        // reset the threshold; an enqueue that returned with the threshold
        // still negative would let a later dequeue answer empty with its
        // element queued (wcq_test helped holds a helper there and
        // catches it). So the enqueue resets it itself before it returns,
        // as SCQ's does.
        reset_threshold();
        st.seq1.store(seq + 1, std::memory_order_seq_cst);
        st.eidx.store(kTerm, std::memory_order_seq_cst);
    }

    // The slow-path enqueue of r's request, by r's thread or a helper
    // (enqueue_slow and try_enq_slow, Figure 7). At each round's position
    // the cooperating threads agree, through CAS2 on the entry, on one of
    // two outcomes: the entry is produced (once, with Enq clear), or it is
    // skipped (its Note set to the round's cycle); the next round starts
    // only after a skip, so no index is produced twice.
    void do_enqueue_slow(word eidx, word seq, word tail, state& r, unsigned me) {
        while (slow_inc(tail_, r.tail, tail, false, me)) {
            if (r.seq1.load(std::memory_order_seq_cst) != seq) return;
            Hooks::slow_round();
            const word tcycle = tail | (4 * n_ - 1);
            pair& slot = entries_[map(tail >> 2)];
            word entry, note;
            Words::load(slot, entry, note);
            for (;;) {
                Hooks::step();
                const word ecycle = entry | (4 * n_ - 1);
                if (ecycle == tcycle) {
                    // This round's cycle: a dequeue that came first left
                    // kBot (the next round), or this request produced it.
#if WTC_TEST_RING_MUTANT != 3  // runtime/test/model: the figures' reading
                    if ((entry & (n_ - 1)) == n_ - 2) break;
#endif
                    return;
                }
                if (!lt(ecycle, tcycle) || !lt(note, tcycle)) break;  // a later cycle, or skipped
                if ((entry | 1) == ecycle ||
                    ((entry | 1) == (ecycle ^ (2 * n_)) && le(Words::lo(head_), tail))) {
                    // Produce it with Enq clear; the thread whose FIN lands
                    // sets Enq (a dequeue that finds it clear sets FIN and
                    // consumes it, which sets Enq too).
                    if (!Words::cas(slot, entry, note, tcycle ^ eidx ^ n_, note)) continue;
                    word t = tail;
                    if (r.tail.compare_exchange_strong(t, tail | kFin, std::memory_order_seq_cst)) {
                        word e = tcycle ^ eidx ^ n_;
                        Words::cas_lo(slot, e, e ^ n_);
                    }
                    Hooks::produced();
                    reset_threshold();
                    return;
                }
                // Unusable at this cycle: every cooperating thread skips it.
                if (!Words::cas(slot, entry, note, entry, tcycle)) continue;
                break;
            }
        }
    }

    bool dequeue_slow(word head, std::uint32_t& index, state& st, unsigned me) {
        slow_.fetch_add(1, std::memory_order_relaxed);
        const word seq = st.seq1.load(std::memory_order_seq_cst);
        st.head.store(head, std::memory_order_seq_cst);
        st.init_head.store(head, std::memory_order_seq_cst);
        st.eidx.store(kDeq, std::memory_order_seq_cst);
        st.seq2.store(seq, std::memory_order_seq_cst);
        do_dequeue_slow(seq, head, st, me);
        st.seq1.store(seq + 1, std::memory_order_seq_cst);
        st.eidx.store(kTerm, std::memory_order_seq_cst);
        return take_result(st, index);
    }

    // A finished slow-path dequeue's result: the entry at the position it
    // finished on.
    bool take_result(state& st, std::uint32_t& index) {
        const word head = st.head.load(std::memory_order_seq_cst);
        const word hcycle = head | (4 * n_ - 1);
        pair& slot = entries_[map(head >> 2)];
        const word entry = Words::lo(slot);
        // FIN on an entry of the position's cycle holding an index: the
        // element (no other dequeue has this position). Otherwise FIN came
        // from an empty ring.
        if ((entry | (4 * n_ - 1)) == hcycle && (entry & (n_ - 1)) < n_ - 2) {
            // The position without the request's flags: the producing
            // request's counter holds the bare position (the published
            // code passes it with FIN, which matches no request).
            if (!(entry & n_)) lookup(st, head & ~word{3});
            Words::or_lo(slot, 2 * n_ - 1);
            index = static_cast<std::uint32_t>(entry & (n_ - 1));
            return true;
        }
        return false;
    }

    // The slow-path dequeue of r's request (try_deq_slow, Figure 7): FIN
    // at the position whose entry is ready, or at one where the ring is
    // found empty.
    void do_dequeue_slow(word seq, word head, state& r, unsigned me) {
        while (slow_inc(head_, r.head, head, true, me)) {
            if (r.seq1.load(std::memory_order_seq_cst) != seq) return;
            Hooks::slow_round();
            const word hcycle = head | (4 * n_ - 1);
            pair& slot = entries_[map(head >> 2)];
            word entry, note;
            Words::load(slot, entry, note);
            for (;;) {
                Hooks::step();
                const word ecycle = entry | (4 * n_ - 1);
                if (ecycle == hcycle && (entry & (n_ - 1)) != (n_ - 2)) {
                    // Ready, or consumed already by a cooperating thread:
                    // this position is the request's result.
                    word h = head;
                    r.head.compare_exchange_strong(h, head ^ kFin, std::memory_order_seq_cst);
                    return;
                }
                word next;
                if ((entry | (2 * n_) | 1) != ecycle) {
                    if (lt(ecycle, hcycle) && lt(note, hcycle)) {
                        // Keep later helpers of this request from it.
                        if (!Words::cas(slot, entry, note, entry, hcycle)) continue;
                    }
                    next = entry & ~(2 * n_);
                    if (entry == next) break;
                } else {
                    next = hcycle ^ ((~entry) & (2 * n_)) ^ 1;
                }
                if (!lt(ecycle, hcycle)) break;
                if (Words::cas(slot, entry, note, next, note)) break;
            }
            const word t = Words::lo(tail_);
            if (le(t, head + 4)) catchup(t, head + 4);
            if (threshold_.load(std::memory_order_seq_cst) < 0) {
                word h = head;
                r.head.compare_exchange_strong(h, head + kFin, std::memory_order_seq_cst);
            }
        }
    }

    // A state whose slot was taken from a thread that had exited. A thread
    // exits between its operations, so its state has no request pending;
    // only a fork's child finds one, left by a thread that did not survive
    // the fork inside a slow path. A request is pending once its seq2 is
    // its seq1 and until seq1 moves on; it is run to its end as its owner
    // would have, so that no helper of it is left to meet the new owner's
    // requests, and a dequeue's element is consumed and lost with the
    // thread (hazard.hpp: an operation inside which a thread was at the
    // fork is abandoned).
    void adopt(state& st, unsigned me) {
        const word eidx = st.eidx.load(std::memory_order_seq_cst);
        if (eidx == kTerm) return;
        const word seq = st.seq1.load(std::memory_order_seq_cst);
        if (st.seq2.load(std::memory_order_seq_cst) == seq) {
            if (eidx > kDeq) {
                do_enqueue_slow(eidx, seq, st.init_tail.load(std::memory_order_seq_cst), st, me);
                reset_threshold();
                st.seq1.store(seq + 1, std::memory_order_seq_cst);
            } else {
                do_dequeue_slow(seq, st.init_head.load(std::memory_order_seq_cst), st, me);
                st.seq1.store(seq + 1, std::memory_order_seq_cst);
                std::uint32_t lost;
                take_result(st, lost);
            }
        }
        st.eidx.store(kTerm, std::memory_order_seq_cst);
    }

    // A thread with no slot: SCQ's loops, without patience.
    void enqueue_fallback(std::uint32_t index) {
        fallbacks_.fetch_add(1, std::memory_order_relaxed);
        const word eidx = word(index) ^ (n_ - 1);
        for (;;) {
            Hooks::step();
            if (try_enq(Words::faa_lo(tail_, 4), eidx)) return;
        }
    }
    bool dequeue_fallback(std::uint32_t& index) {
        fallbacks_.fetch_add(1, std::memory_order_relaxed);
        for (;;) {
            Hooks::step();
            switch (try_deq(Words::faa_lo(head_, 4), index, states_[0])) {
            case kOk: return true;
            case kEmpty: return false;
            default: break;
            }
        }
    }

    const unsigned order_;
    const word n_;  // entries: 2 x indices
    const unsigned nstates_;
    pair* const entries_;
    state* const states_;
    Ids slots_;
    alignas(64) pair head_;
    alignas(64) pair tail_;
    alignas(64) std::atomic<sword> threshold_{-1};
    alignas(64) std::atomic<std::uint64_t> fallbacks_{0};
    std::atomic<std::uint64_t> slow_{0};
};

// The wait-free ring with its defaults, as index_queue's Ring.
template <class Hooks>
using wcq_default = wcq_ring<Hooks>;

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
          fq_(order_, cells_n_, threads),
          aq_(order_, 0, threads) {
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

    // The rings, for tests: the free cells' and the queued elements'.
    const Ring<Hooks>& free_ring() const { return fq_; }
    const Ring<Hooks>& queued_ring() const { return aq_; }

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
