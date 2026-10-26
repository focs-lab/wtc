# 3. What a wtc operation promises about concurrency

Proposed 2026-10-18. Taken 2026-10-19: option 2, the minimal contract
below, written into the description of each queue operation in
`mlir/include/WTC/WTCOps.td`. The last question, at the end, stays open.

## Context

The four ops now have types, verifiers, and a checked `fallback_impl`, and
each declares that it reads and writes memory. That last part says nothing
about concurrency. It makes every op an opaque barrier, which is safe for
transforms around it, and it is the only thing the IR says. An op still means
"a call to this function, recognised".

That matters at substitution, not before. `--wtc-specialize` switches a
`wtc::queue<T>`, which takes a mutex on every operation, to a lock-free
ring that synchronises through acquire and release. Whether that is a
correct substitution depends on what the program was entitled to rely on,
and today nothing writes that down. The answer is currently "whatever the
mutex happened to give", which is more than the substitute gives.

## The questions

**Ordering.** Every implementation must give the per-item guarantee: a push
happens-before the pop that returns its item. The mutex-based queue also
totally orders all operations on one queue, and orders them against the
program's other uses of mutexes. A program that uses an empty pop as a
signal ("the producer has not started yet") relies on that total order. The
SPSC substitute does not give it.

**Linearisability and FIFO.** Both implementations are linearisable when used
as SPSC, and both are FIFO. With several producers, the only order defined
is each producer's own.

The list substituted for many producers and one consumer from 21 to
22 Oct 2026 (now `benchmarks/queues/mpsc_list_original.hpp`, a baseline;
the update below says what replaced it) ordered the pushes by the exchange
that publishes each, so a push that happens before another comes out
first. Its pop can report empty while a push that started earlier is
between its two steps, even when later pushes have finished; a consumer
that spins on the pop sees them once it finishes.

The queue substituted for one producer and many consumers
(`runtime/spmc_queue.hpp`, added later still) is a list of segments the
producer fills and publishes alone, whose cells the consumers claim with a
compare-and-swap against the published count: FIFO in the producer's order
and linearisable, and a pop that reports empty does so when every published
element has been popped. Until 26 Oct 2026 it was the SPSC ring with its
consumers taking turns under a spin lock, which serialised every pop's
transfers and ran a pool at half the general queue's rate.

**Progress.** The shim queue blocked on a mutex; what each substitute
guarantees is in `decisions/0004` (the rings wait-free, the segmented
queue lock-free with a pop that waits for a reserved cell, the spmc
queue's pop lock-free), and every one allocates a block or a segment
when it grows. The element itself lives in the cell, on every
implementation (since 23 Oct 2026): a push copies or moves it there, and a
pop moves it out, lends it in place, or allocates the `shared_ptr` the
original API returns on the consumer's side, so that the memory that
crosses the threads is the queue's own and no allocation does. A program
cannot observe progress except as deadlock or starvation, and a
substitution can only make those less likely, not more.

**Effects.** Per-object memory effects would let a transform reorder ops on
two different queues. In a concurrent program that is not generally sound,
since an op on one queue can be what orders an op on the other. The single
default resource is right until a model says otherwise.

## Options

1. **Keep "opaque call".** Nothing is promised and nothing is checked. Every
   substitution is then justified only by its author's reading of the
   program.
2. **Promise the minimal contract, per op kind, in the dialect's
   documentation.** That is: per-item happens-before from a push to its pop;
   linearisability per queue; FIFO per producer; no ordering against other
   objects; no progress guarantee. A substitute's header states it meets the
   contract. A program relying on more — empty-pop signalling, ordering
   across queues — is outside what the dialect promises, and is not a
   candidate for substitution.
3. **Carry an ordering attribute on each op**, modelled on LLVM's atomic
   orderings, so that a stronger guarantee can be asked for where a program
   needs it, and verified against the implementation chosen.

## Proposal

Option 2 now, with room for option 3 later. It costs a precise paragraph per
op and a promise in each `runtime/` header, and it turns "is this
substitution correct" into a question with a written answer.

What it does not settle, and what the discussion should: whether the
analysis has to detect programs that rely on more than the contract (for
example, a pop whose emptiness is branched on and then followed by an
access to other shared state), or whether those are accepted as the
programmer's responsibility, as they would be for any library swap.

## Update, 22 Oct 2026: the general-purpose queue is no longer a mutex

The general-purpose implementation, which every queue the analysis does not
prove keeps, became the segmented queue (`runtime/segment_queue.hpp`), and
its single-consumer form replaced the list for many producers. It meets the
contract above and promises nothing beyond it:

- every push happens-before the pop that returns its element;
- it is linearisable, so a push that finishes before another starts comes
  out first, and a pop that finds the queue empty does so when every push
  that finished before it began has been popped: with many consumers a pop
  waits for a cell a push has taken and not yet filled, and the single
  consumer takes the first full cell after it instead (update below);
- it is FIFO per producer, and a producer's batch comes out in its order;
- it orders nothing against other objects. A program that relied on the
  mutex of the original queue to order its operations against its other
  mutexes, the empty-pop signal of the Context above among them, is
  outside the contract, as it already was for every substitute.

This moves the line the contract draws from the substitutes to the program
as written. The original two-lock queue stays in the tree as a baseline
(`benchmarks/queues/generic_original.hpp`), not as an implementation the
shim offers.

## Update, 26 Oct 2026: the single consumer's empty answer

Until this date the single-consumer form answered empty at the first cell
that was not full, as the list before it did, even when a later push had
finished. That is no linearisation of a queue, and the general-purpose
queue, whose pop waits for such a cell, never gives it, so the
substitution of `mpsc` could add an empty answer to the program: a
producer pushes and then sets a flag, and the only consumer, having seen
the flag, pops while another producer is between taking an earlier cell
and filling it. The general-purpose queue returns an element there; the
`mpsc` form returned empty. It now takes the earliest full cell past the
unfilled one, as the Jiffy queue's dequeue does, and stays wait-free
(`runtime/segment_queue.hpp`, `pop_one`;
`runtime/test/segment_queue_empty_test.cpp`).

What the contract now asks of a producer changes with it. A producer that
stops for ever between taking its cell and filling it (a thread killed
there, or one that never runs again) used to leave the consumer answering
empty for ever at that cell; now the consumer goes on past it, and the
cell keeps the head where it is, so no segment after it is freed and the
queue's memory grows with every element pushed after it.
