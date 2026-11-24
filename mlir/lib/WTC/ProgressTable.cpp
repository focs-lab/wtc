#include "WTC/ProgressTable.h"

// The table (docs/progress.md, "The table"). Every row is read from the
// runtime's code; the evidence is in the comments, by file and function,
// since line numbers move.
//
// Steps: one access to shared memory, or one pause of a bounded wait. The
// constants named in a bound are the runtime's: the consumer's slip
// (runtime/spsc_ring.hpp, kSlipPauses + kSlipGapMax = 64 + 32 pauses;
// runtime/segment_queue.hpp, 64 + 4), the segment of 1024 cells, a hazard
// record's slots (runtime/hazard.hpp, kSlots = 7) and the retired nodes a
// scan waits for (kScanAt = 8), and the threads a ring of indices is built
// for (shim/bounded.hpp, kIndexThreads = 128).
//
// No queue operation of this runtime takes a static-initialisation guard:
// the tick rate the slip and the waiters read is measured when the first
// queue or waiter is built (runtime/stream_gate.hpp, ensure_ticks_per_ms),
// and read with one relaxed load. The first operation of a thread on a
// queue that uses hazard pointers takes a record (hazard::tied_record):
// one compare-and-swap or fetch-and-add and a pthread_setspecific, which
// allocates unless the key is among the C library's first 32 and fewer
// than 256 threads hold records at once; first_use_tls.

namespace mlir::wtc {

namespace {

constexpr uint64_t kSegmentCells = 1024;
constexpr uint64_t kRingSlipPauses = 64 + 32;
constexpr uint64_t kSegmentSlipPauses = 64 + 4;
constexpr uint64_t kIndexThreads = 128;

bool ring(Impl impl) { return impl == Impl::spsc || impl == Impl::spsc_inline; }
bool settledRing(Impl impl) {
    return impl == Impl::spsc_settled || impl == Impl::mpsc_settled || impl == Impl::spmc_settled;
}

// What an implementation means on a form it does not serve: the bounded
// rings of indices and the settled rings serve only bounded forms, jiffy
// only unbounded ones. A queue built to overrun, or a C queue's batches,
// keep the bounded ring whatever the tag (shim/bounded.hpp: the settled
// order needs the split layout, and the rings of indices neither overrun
// nor take batches). Anything else is read as the general-purpose
// implementation, the answer that is never too optimistic.
Impl effective(OpForm form, Impl impl) {
    const bool bounded = isBoundedForm(form);
    const bool newer = impl == Impl::scq || impl == Impl::wcq || settledRing(impl);
    if (newer && !bounded) return Impl::generic;
    if (newer && (form == OpForm::bounded_push_overrun || form == OpForm::bounded_push_batch ||
                  form == OpForm::bounded_pop_batch))
        return Impl::generic;
    if (impl == Impl::jiffy && bounded) return Impl::generic;
    return impl;
}

// A retire and the scan it may start (hazard::retire, hazard::scan): the
// scan reads every slot of the n records handed out, and for each slot
// that holds a pointer walks the retired list, at most kScanAt plus the
// nodes held at the last scan, one for each such slot; then it frees what
// no slot holds. 7·n reads, 7·n·(8 + 7·n) looks, 8 + 7·n frees. A retire
// of the one-counter form may walk further, past a push's mark.
Term scanTerms[] = {Term{49, 0, 0, 2, {}}, Term{70, 0, 0, 1, {}}, Term{8, 0, 0, 0, {}}};

ProgressSpec spec(Level level, Bound bound, Alloc alloc, std::string reason) {
    ProgressSpec s;
    s.level = level;
    s.bound = level == Level::wait_free ? std::move(bound) : Bound::none();
    s.alloc = alloc;
    s.reason = std::move(reason);
    return s;
}

// spsc_ring::try_pop: an acquire load of the producer's count; the slip,
// at most kSlipPauses polls of that count with at most kSlipPauses +
// kSlipGapMax pauses between them; at a block's end a load of the next
// block and a release store of the consumer's count.
Bound ringPop() {
    return Bound::poly({Term{1 + 64 + 2, 0, 0, 0, {}}, Term{kRingSlipPauses, 0, 0, 0, "slip pauses"}});
}

const char *kBlockFull = "allocates a new block when the current one is full: the queue is unbounded";
const char *kSegmentFull = "allocates a new segment when the current one is full: the queue is unbounded";
const char *kNoteHoles = "may allocate to note the cells it passes that a push has taken and not filled";

// A producer's step on an unbounded queue: push, push_deferred, and the
// end of a run of them. Every publish is followed by the waiter's notify
// (waiter::notify): a fence and a load, and a FUTEX_WAKE when a consumer
// sleeps, on a queue whose policy can park: two steps, counted.
ProgressSpec unboundedPush(OpForm form, Impl impl, const ThreadCounts &counts) {
    ProgressSpec s;
    if (form == OpForm::batch_begin) return spec(Level::wait_free, Bound::zero(), Alloc::never(), "");
    constexpr uint64_t kNotify = 2;
    const bool publishes = form != OpForm::push_deferred;
    switch (impl) {
    case Impl::spsc:
    case Impl::spsc_inline:
        // spsc_ring::push_deferred: the cell's construction, and once per
        // block turn_over: a relaxed load of the next block, an acquire load
        // of the consumer's count, and two stores to link a new block;
        // publish: one release store.
        if (form == OpForm::batch_end) {
            s = spec(Level::wait_free, Bound::constant(1 + kNotify), Alloc::never(), "");
            break;
        }
        s = spec(Level::wait_free, Bound::constant(5 + (publishes ? 1 + kNotify : 0)), Alloc::amortised(0), kBlockFull);
        s.element = element_copy_ctor | element_move_ctor;
        break;
    case Impl::spmc:
        // spmc_queue::push_deferred: the cell's construction, and at a full
        // segment two release stores; publish: one release store.
        if (form == OpForm::batch_end) {
            s = spec(Level::wait_free, Bound::constant(1 + kNotify), Alloc::never(), "");
            break;
        }
        s = spec(Level::wait_free, Bound::constant(3 + (publishes ? 1 + kNotify : 0)), Alloc::amortised(kSegmentCells),
                 kSegmentFull);
        s.element = element_copy_ctor | element_move_ctor;
        break;
    case Impl::jiffy: {
        // segment_queue::push_one of the one-counter form: one
        // fetch-and-add on the queue's one counter, then, each time it
        // protects the tail, at most kProtectTries + 1 = 5 reads and one
        // store and read of protect_all, and the walk to the segment of its
        // index: the segments only the pushes in flight put between the tail
        // and the index when it is not preempted between reading the tail
        // and its fetch-and-add, and the segments linked meanwhile when it
        // is. Wait-free; no bound in the thread counts is claimed for the
        // second case (runtime/README.md).
        s = spec(Level::wait_free,
                 Bound::history("the segments it walks to its index's are those linked while it was preempted, "
                                "not bounded in the counts"),
                 Alloc::amortised(kSegmentCells),
                 form == OpForm::batch_end
                     ? "allocates a new segment when it publishes the run past the last one"
                     : "allocates a new segment when its index is past the last one: the queue is unbounded");
        s.firstUse = first_use_tls;
        s.element = element_copy_ctor | element_move_ctor | element_move_assign | element_dtor;
        if (form == OpForm::batch_end) s.element = element_move_ctor | element_move_assign | element_dtor;
        break;
    }
    case Impl::mpsc:
    case Impl::generic:
    default: {
        // segment_queue::push: the element is built into a temporary, moved
        // into its cell from there by push_run, and the temporary destroyed;
        // a push that loses its cell (a pop skipped it, or another push
        // linked the segment first) takes the element back by
        // move-assignment and tries again: lock-free. With one producer no
        // other push takes an index or links a segment, so push_run runs at
        // most twice: each time hazard::protect (a load, a store, a load),
        // the fetch-and-add and the cell's state store; at the segment's
        // end, up to 64 polls and pauses for a link that only this producer
        // makes, then the new segment and two compare-and-swaps; and the
        // hazard slot's clear (mpsc only: the general-purpose queue's pops
        // may still skip its cells). A run from a batch (batch_end, or
        // push_deferred at its WTC_BATCH_K-th element) is the same with the
        // run's elements moved in and destroyed after.
        const bool alone = impl == Impl::mpsc && counts.producers.atMostOne();
        const char *why = form == OpForm::batch_end ? "allocates a new segment when it publishes the run into a full one"
                          : form == OpForm::push_deferred
                              ? "allocates a new segment when it publishes into a full one: the queue is unbounded"
                              : kSegmentFull;
        s = spec(alone ? Level::wait_free : Level::lock_free,
                 Bound::poly({Term{2 * 5 + 1 + 64 + 3 + kNotify, 0, 0, 0, {}}, Term{64, 0, 0, 0, "segment-end pauses"}}),
                 Alloc::amortised(kSegmentCells), why);
        // A run of a batch publishes up to WTC_BATCH_K elements at once.
        if (form != OpForm::push) s.bound = alone ? Bound::finite("a run of up to WTC_BATCH_K elements") : Bound::none();
        // hazard::mine: the first operation of each thread registers a
        // thread-local record (runtime/hazard.hpp).
        s.firstUse = first_use_tls;
        s.element = element_copy_ctor | element_move_ctor | element_move_assign | element_dtor;
        if (form == OpForm::batch_end) s.element = element_move_ctor | element_move_assign | element_dtor;
        break;
    }
    }
    if (form == OpForm::push_deferred && ring(impl)) s.reason = kBlockFull;
    if (form == OpForm::push_deferred && impl == Impl::spmc) s.reason = kSegmentFull;
    // A queue whose wait policy can park wakes a sleeping consumer after a
    // publish (waiter::notify): a fence, a load and a FUTEX_WAKE.
    s.kernel = Kernel::futex_wake;
    return s;
}

// A consumer's step on an unbounded queue, not waiting.
ProgressSpec unboundedPop(bool shares, Impl impl, const ThreadCounts &counts) {
    const char *sharedPtr = shares ? ", and allocates the shared_ptr it returns" : "";
    ProgressSpec s;
    switch (impl) {
    case Impl::spsc_inline:
        // spsc_ring::try_pop, lent: nothing allocated, the element
        // destroyed in place at the next pop.
        s = spec(Level::wait_free, ringPop(), Alloc::never(), "");
        s.element = shares ? element_dtor : element_move_assign | element_dtor;
        return s;
    case Impl::spsc:
        // spsc_ring::try_pop: a bounded slip of at most kSlipPauses +
        // kSlipGapMax pauses, timed with the tick rate measured when the
        // queue was built.
        s = spec(Level::wait_free, ringPop(), shares ? Alloc::always() : Alloc::never(),
                 shares ? "allocates the shared_ptr it returns" : "");
        break;
    case Impl::spmc: {
        // spmc_queue::try_pop: the claim's compare-and-swap fails only to
        // another consumer's success; with one consumer it never fails.
        // Retire and scan allocate nothing (an intrusive list), but the
        // scan frees the segments no slot holds.
        // One consumer: the head's seq_cst load and its hazard slot (a
        // load, or protect's load, store and load), deq, seen and enq
        // loads, the seen store, the claim, at a drained segment the next
        // load and the head's compare-and-swap, twice; and a retire with
        // its scan (scanTerms).
        const bool alone = counts.consumers.atMostOne();
        s = spec(alone ? Level::wait_free : Level::lock_free,
                 Bound::poly({Term{15, 0, 0, 0, {}}, scanTerms[0], scanTerms[1], scanTerms[2]}), Alloc::always(),
                 std::string("frees the segments it moves past") + sharedPtr);
        s.firstUse = first_use_tls;
        break;
    }
    case Impl::mpsc:
        // segment_queue::try_pop, one consumer: it never waits for a
        // thread. It looks at each of its h holes up to h times and erases
        // the one it takes: h² + h steps (pop_one). The walk ahead adds
        // holes only when none of them is full, so when it walks every hole
        // belongs to a push still in flight; h is at most the cells those
        // pushes reserve, plus the full cell the walk stops at. A producer
        // thread has at most 1 + kNestedPushes = 5 pushes in flight at once
        // (an outer one and four nested in its elements' code, hazard.hpp),
        // each reserving up to B = WTC_BATCH_K = 64 cells (a batch's run),
        // so h <= 320·np + 1 and h² + h <= 102400·np² + 960·np + 2. The slip
        // polls at most 64 times and pauses 64 + 4. When the earliest hole
        // is taken the head moves past the cells taken meanwhile at once and
        // the pop retires at most one segment (move_head, retire_one), so
        // the bound holds for every call: the looks, the slip, and one
        // retire with its scan (scanTerms, 8 of whose steps are in the
        // constant).
        s = spec(Level::wait_free,
                 Bound::poly({Term{102400, 2, 0, 0, {}}, Term{960, 1, 0, 0, {}}, scanTerms[0], scanTerms[1],
                              Term{2 + 64 + 4 + 8, 0, 0, 0, {}}, Term{kSegmentSlipPauses, 0, 0, 0, "slip pauses"}}),
                 Alloc::always(), std::string("frees the segments it moves past, and ") + kNoteHoles + sharedPtr);
        s.firstUse = first_use_tls;
        break;
    case Impl::jiffy:
        // segment_queue::try_pop of the one-counter form: the mpsc pop, and
        // it folds the segments past a stall once their cells are taken.
        // Its retire's scan also walks, while a push holds its queue's mark
        // (hazard::protect_all), the segments the queue retired since the
        // oldest such mark: at most the queue's backlog when that push took
        // its index, which a preempted push keeps for as long as it is
        // preempted. Bounded in each run, not in the counts.
        s = spec(Level::wait_free,
                 Bound::history("a retire's scan walks the segments retired since the oldest mark a push holds, "
                                "up to the queue's backlog when that push took its index"),
                 Alloc::always(), std::string("frees the segments it moves past, and ") + kNoteHoles + sharedPtr);
        s.amortised = Bound::poly({Term{102400, 2, 0, 0, {}}, Term{960, 1, 0, 0, {}}, scanTerms[0], scanTerms[1],
                                   Term{2 + 64 + 4 + 8, 0, 0, 0, {}}, Term{kSegmentSlipPauses, 0, 0, 0, "slip pauses"}});
        s.firstUse = first_use_tls;
        break;
    case Impl::generic:
    default:
        // segment_queue::try_pop, many consumers: at a cell a push has
        // reserved and not filled, it spins and then yields without bound.
        s = spec(Level::blocking_sf, Bound::none(), Alloc::always(),
                 std::string("waits for a push that has taken the cell it reads to fill it, and frees the segments it "
                             "moves past") +
                     sharedPtr);
        s.firstUse = first_use_tls;
        break;
    }
    s.element = shares ? element_move_ctor | element_dtor : element_move_assign | element_dtor;
    return s;
}

// A waiting pop: the eventcount waits for data or for close(), under spin
// by polling, under park asleep in FUTEX_WAIT (runtime/waiter.hpp). Waiting
// for data is the dual operation's request; the follow-up is the pop above.
ProgressSpec unboundedPopWait(bool timeout, Impl impl, const ThreadCounts &counts) {
    ProgressSpec s = unboundedPop(/*shares=*/false, impl, counts);
    s.partial = true;
    s.kernel = Kernel::futex_wait;
    std::string why = timeout ? "waits for a push, for the queue to be closed, or for its time limit, as its wait "
                                "policy says: spinning, or asleep in the kernel"
                              : "waits for a push or for the queue to be closed, as its wait policy says: spinning, or "
                                "asleep in the kernel";
    switch (impl) {
    case Impl::generic: why += "; and frees the segments it moves past"; break;
    case Impl::mpsc: why += std::string("; and frees the segments it moves past, and ") + kNoteHoles; break;
    case Impl::spmc: why += "; and frees the segments it moves past"; break;
    case Impl::jiffy: why += std::string("; and frees the segments it moves past, and ") + kNoteHoles; break;
    default: break;
    }
    s.reason = why;
    return s;
}

// The bounded queues (shim/bounded.hpp).
//
// The bounded ring (runtime/bounded_ring.hpp) takes a ticket and then
// waits for the previous lap's pop to free its cell (fill), and a pop
// waits for the push of its ticket to fill it (take), even with one
// producer and one consumer: blocking on every topology.
//
// Built settled, a single side fills or empties its cell before it
// publishes its counter, so the other side never waits for it: with one
// producer no pop waits (bounded_ring::try_push: tail and head loads, the
// cell's sequence load, the construction, the sequence and tail stores;
// one more tail load when full), with one consumer no push waits
// (bounded_ring::consume: head, tail_seen and tail loads, the tail_seen
// store, the cell's sequence load, the move, the sequence and head
// stores). The many side takes its tickets with a compare-and-swap that
// fails only to another of its own: lock-free, wait-free when its count
// is one. The wait of a single side for a many side remains: an
// mpsc_settled pop waits for the push of its ticket, an spmc_settled push
// for the previous lap's pop of its cell.
//
// The rings of indices (runtime/index_ring.hpp, the exact variant): no
// operation waits for another thread and nothing allocates after
// construction. scq is lock-free. wcq is wait-free while every thread
// operating on it holds one of its kIndexThreads thread slots and the
// double-width compare-and-swap is one instruction (the shim announces
// wcq only then); past the slots a thread runs SCQ's loops and the
// others are then not wait-free either, so the row is lock-free unless
// the counts show both sides within kIndexThreads. Two more threads take
// no slot and run those loops, which the counts cannot see and the row
// states in its bound's note (runtime/README.md): one whose hazard record
// could not be tied to its exit (the pthread key's allocation failed,
// until it is), and an operation from a key destructor that runs after
// its thread's exit. Its paper proves the steps finite and gives no
// closed form. A thread's first operation on a wcq queue takes a hazard
// record and a thread slot.
ProgressSpec bounded(OpForm form, Impl impl, const ThreadCounts &counts) {
    const bool producer = form == OpForm::bounded_try_push || form == OpForm::bounded_push_wait ||
                          form == OpForm::bounded_push_overrun || form == OpForm::bounded_push_batch;
    const bool waits = form == OpForm::bounded_push_wait || form == OpForm::bounded_push_batch ||
                       form == OpForm::bounded_pop_wait || form == OpForm::bounded_pop_wait_for ||
                       form == OpForm::bounded_pop_batch;
    const bool shares = form == OpForm::bounded_pop_shared;
    const Alloc alloc = shares ? Alloc::always() : Alloc::never();
    const char *sharedPtr = shares ? "allocates the shared_ptr it returns" : "";
    constexpr uint64_t kNotify = 2;
    ProgressSpec s;
    std::string why;
    if (impl == Impl::scq || impl == Impl::wcq) {
        std::optional<uint64_t> threads;
        if (auto p = counts.producers.value())
            if (auto c = counts.consumers.value()) threads = *p + *c;
        const bool wf = impl == Impl::wcq && threads && *threads <= kIndexThreads;
        if (wf)
            s = spec(Level::wait_free,
                     Bound::finite("wCQ's steps are finite (SPAA'22) and its paper gives no closed form; while no "
                                   "thread runs SCQ's loops for want of a slot: past 128, untied from its hazard "
                                   "key, or from a key destructor after its exit"),
                     alloc,
                     sharedPtr);
        else
            s = spec(Level::lock_free, Bound::none(), alloc,
                     impl == Impl::wcq ? "lock-free unless the threads operating on it are shown within its thread "
                                         "slots (128), past which they run SCQ's loops"
                                       : sharedPtr);
        if (shares && impl == Impl::wcq && !wf) s.reason += ", and allocates the shared_ptr it returns";
        if (impl == Impl::wcq) s.firstUse = first_use_tls;
    } else if (settledRing(impl)) {
        const bool oneProducer = impl == Impl::spsc_settled || impl == Impl::spmc_settled;
        const bool oneConsumer = impl == Impl::spsc_settled || impl == Impl::mpsc_settled;
        // Does this side wait for the other? Only a single side waits, and
        // only for a many side.
        const bool waitsForOther = producer ? !oneConsumer : !oneProducer;
        if (waitsForOther) {
            why = producer ? "may wait for the pop that last held its cell to finish moving the item out"
                           : "may wait for the push that took its cell to fill it";
            s = spec(Level::blocking_sf, Bound::none(), alloc, why);
        } else {
            const bool single = producer ? oneProducer : oneConsumer;
            const Count &mine = producer ? counts.producers : counts.consumers;
            // try_push: 6 accesses, one more tail load when full, and the
            // notify of the other side's waiter; consume: 8 and the
            // notify. A many side's failed compare-and-swap repeats them.
            const uint64_t steps = (producer ? 7 : 8) + kNotify;
            if (single || mine.atMostOne())
                s = spec(Level::wait_free, Bound::constant(steps), alloc, sharedPtr);
            else
                s = spec(Level::lock_free, Bound::none(), alloc, sharedPtr);
        }
    } else {
        why = producer ? "may wait for the pop that last held its cell to finish moving the item out"
                       : "may wait for the push that took its cell to fill it";
        s = spec(Level::blocking_sf, Bound::none(), alloc, why);
    }
    if (waits) {
        s.partial = true;
        s.kernel = Kernel::futex_wait;
        if (!why.empty()) {
            if (producer)
                s.reason = "waits while the queue is full, as its wait policy says; and " + why;
            else
                s.reason = (form == OpForm::bounded_pop_wait_for
                                ? std::string("waits for a push, for the queue to be closed, or for its time limit")
                                : std::string("waits for a push or for the queue to be closed")) +
                           ", as its wait policy says; and " + why;
        } else {
            s.reason = producer ? "waits while the queue is full, as its wait policy says"
                                : "waits for a push or for the queue to be closed, as its wait policy says";
        }
    } else if (producer || !shares) {
        // Every publish may wake a sleeping peer (bounded::try_push,
        // made_room).
        s.kernel = Kernel::futex_wake;
    }
    if (producer) {
        s.element = form == OpForm::bounded_push_batch ? element_copy_ctor : element_copy_ctor | element_move_ctor;
        // An overrun destroys the oldest element to make room.
        if (form == OpForm::bounded_push_overrun) s.element |= element_dtor;
    } else {
        s.element = shares ? element_move_ctor | element_dtor : element_move_assign | element_dtor;
    }
    return s;
}

} // namespace

ProgressSpec progressSpecFor(OpForm form, Impl rawImpl, const ThreadCounts &counts) {
    const Impl impl = effective(form, rawImpl);
    switch (form) {
    case OpForm::push:
    case OpForm::push_deferred:
    case OpForm::batch_begin:
    case OpForm::batch_end:
        return unboundedPush(form, impl, counts);
    case OpForm::pop_shared:
        return unboundedPop(/*shares=*/true, impl, counts);
    case OpForm::pop_transfer:
        return unboundedPop(/*shares=*/false, impl, counts);
    case OpForm::pop_wait:
        return unboundedPopWait(/*timeout=*/false, impl, counts);
    case OpForm::pop_wait_for:
        return unboundedPopWait(/*timeout=*/true, impl, counts);
    case OpForm::bounded_try_push:
    case OpForm::bounded_push_wait:
    case OpForm::bounded_push_overrun:
    case OpForm::bounded_push_batch:
    case OpForm::bounded_pop_shared:
    case OpForm::bounded_pop_transfer:
    case OpForm::bounded_pop_wait:
    case OpForm::bounded_pop_wait_for:
    case OpForm::bounded_pop_batch:
        return bounded(form, impl, counts);
    // std::mutex (shim/mutex.hpp): deadlock-free, not starvation-free.
    case OpForm::lock:
    case OpForm::lock_batch_begin:
        return spec(Level::blocking_df, Bound::none(), Alloc::never(), "takes a mutex");
    case OpForm::unlock:
    case OpForm::lock_batch_end:
        return spec(Level::blocking_df, Bound::none(), Alloc::never(),
                    "releases a mutex, which is half of a blocking protocol");
    case OpForm::lock_batch_step:
        return spec(Level::blocking_df, Bound::none(), Alloc::never(),
                    "lets a mutex go and takes it again every WTC_LOCK_BATCH_K sections");
    // The client waits for the server's done flag, and the server may be
    // preempted or stuck in a section; the section may do what it did as
    // written, the allocator included (runtime/delegate.hpp).
    case OpForm::delegate: {
        ProgressSpec s = spec(Level::blocking_df, Bound::none(), Alloc::always(),
                              "waits for the delegation server of a mutex to run a critical section");
        s.kernel = Kernel::futex_wait;
        return s;
    }
    }
    return spec(Level::blocking_df, Bound::none(), Alloc::always(), "is not in the progress table");
}

llvm::SmallVector<Impl, 8> implsFor(OpForm form) {
    if (!isQueueForm(form)) return {Impl::generic};
    if (isBoundedForm(form))
        return {Impl::generic,      Impl::spsc,         Impl::mpsc,        Impl::spmc,        Impl::scq,
                Impl::wcq,          Impl::spsc_settled, Impl::mpsc_settled, Impl::spmc_settled};
    return {Impl::generic, Impl::spsc, Impl::spsc_inline, Impl::mpsc, Impl::spmc, Impl::jiffy};
}

llvm::SmallVector<ThreadCounts, 4> countsFor(Impl impl) {
    const Count one = Count::one(), many = Count::unbounded();
    auto tc = [](Count p, Count c) { return ThreadCounts{p, c, Count::unbounded()}; };
    switch (impl) {
    case Impl::spsc:
    case Impl::spsc_inline:
    case Impl::spsc_settled:
        return {tc(one, one)};
    case Impl::mpsc:
    case Impl::jiffy:
    case Impl::mpsc_settled:
        return {tc(one, one), tc(many, one)};
    case Impl::spmc:
    case Impl::spmc_settled:
        return {tc(one, one), tc(one, many)};
    default:
        return {tc(one, one), tc(many, one), tc(one, many), tc(many, many)};
    }
}

} // namespace mlir::wtc
