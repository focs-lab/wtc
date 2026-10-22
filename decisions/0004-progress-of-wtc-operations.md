# 4. What "lock-free" means when the compiler checks it

Taken 2026-10-19.

## Context

A function can be declared lock-free (`WTC_LOCK_FREE`, `shim/progress.hpp`),
and `--wtc-check-progress` holds it to that. Two things had to be settled:
what each wtc operation guarantees, which depends on the implementation
chosen for it, and what the check itself establishes.

## What an operation guarantees

Each operation answers two questions through `ProgressOpInterface`: can its
own algorithm wait for another thread, and may it call the allocator. They
are kept apart because they fail differently -- a ring that never waits can
still reach `malloc`, which can -- but the check treats a yes to either as
blocking. The answers, by the implementation recorded on the operation
(`#wtc.impl`; none means general-purpose):

| operation | generic | spsc | spsc_inline | mpsc | spmc |
|---|---|---|---|---|---|
| push | lock-free; allocates the element and new segments | wait-free; allocates the element and new blocks | wait-free; allocates a new block when one fills | lock-free; allocates the element and new segments | wait-free; allocates the element and new blocks |
| pop | blocks: waits for a push that has taken its cell; frees drained segments | wait-free | wait-free | wait-free; frees the segments it moves past | blocks: the consumers' spin lock |
| lock, unlock | blocks | | | | |

The generic and mpsc columns are as of 22 Oct 2026, when both became the
segmented queue (`runtime/segment_queue.hpp`); until then the general-purpose
queue blocked on its mutexes on both sides and the mpsc list's push was one
exchange. A push to either retries when a pop skipped its cell or another
push is linking the next segment, hence lock-free rather than wait-free.

A push to any of these queues can allocate, because they are unbounded; no
producer passes the check. That is the honest answer, not a defect of the
check. A pop from an spsc queue is wait-free, but the shared_ptr it returns
is allocated by the pop itself, from the element in the ring's slot, and its
release frees it: the check reports the allocation and follows the
destructor. A pop from an spsc_inline queue returns a shared_ptr that owns
nothing, so its members are not followed.

## What the check establishes

That nothing reachable from the function blocks: no blocking operation, no
known blocking call (the allocator, the threading library's waits, input
and output, sleeping, throwing), and no call it cannot see into -- a
function without a body here, or an indirect call whose targets are not
known -- since what cannot be seen cannot be cleared.

That is a necessary condition for lock-freedom, not a proof of it. A loop
that spins on an atomic until another thread sets it is not judged: it
reaches nothing blocking and is still not lock-free. Proving progress of a
loop is a different analysis, and the bounded waiting the project's
proposal describes is where it belongs.

## Options not taken

- **Report every blocking path.** More complete, and noisier than a first
  error should be: one chain names the problem, and fixing it shows the
  next. Kept to the first found per function.
- **Trust functions without a body.** Most are harmless, and the ones that
  are not are the ones that matter. Only a short list that touches nothing
  but its arguments' memory (`memcpy`, `memset`, `strlen` and the like) is
  trusted.
