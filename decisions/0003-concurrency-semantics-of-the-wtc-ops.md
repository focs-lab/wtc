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

The list substituted for many producers and one consumer
(`runtime/mpsc_list.hpp`, added later) orders the pushes by the exchange
that publishes each, so a push that happens before another comes out
first. Its pop can report empty while a push that started earlier is
between its two steps, even when later pushes have finished; a consumer
that spins on the pop sees them once it finishes.

The ring substituted for one producer and many consumers
(`runtime/spmc_ring.hpp`, added later still) is the SPSC ring with its
consumers taking turns under a spin lock: FIFO in the producer's order and
linearisable, and a pop that reports empty does so when every published
element has been popped.

**Progress.** The shim queue blocks on a mutex; the substitute is lock-free
on the consumer side, and its producer allocates. A program cannot observe
progress except as deadlock or starvation, and a substitution can only make
those less likely, not more.

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
