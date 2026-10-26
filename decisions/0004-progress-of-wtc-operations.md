# 4. What "lock-free" means when the compiler checks it

Taken 2026-10-19.

Revised 2026-10-26: one chain per blocking site instead of one per
function, and inline assembly that cannot wait is cleared (both under
"What the check establishes"); the table follows the elements held by
value in every implementation since 24 Oct.

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
| push | lock-free; allocates a new segment when one fills | wait-free; allocates a new block when one fills | wait-free; allocates a new block when one fills | lock-free; allocates a new segment when one fills | wait-free; allocates a new block when one fills |
| pop | blocks: waits for a push that has taken its cell; frees drained segments | wait-free | wait-free; lends the element, allocates nothing | wait-free; frees the segments it moves past | blocks: the consumers' spin lock |
| lock, unlock | blocks | | | | |

The generic and mpsc columns are as of 22 Oct 2026, when both became the
segmented queue (`runtime/segment_queue.hpp`); until then the general-purpose
queue blocked on its mutexes on both sides and the mpsc list's push was one
exchange. A push to either retries when a pop skipped its cell or another
push is linking the next segment, hence lock-free rather than wait-free.

Every implementation holds the element itself in its cell, so a push
allocates only when the queue grows. A pop in the shared_ptr form
(`try_pop()`) allocates the shared_ptr it returns, on the consumer's side,
on every implementation but spsc_inline; a pop that moves the element out
(`try_pop(T&)`) allocates nothing.

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

How it reports: one chain for each blocking site of the declared
function's own body -- each call or operation in it that reaches something
blocking -- with the first chain found below that site, deduplicated by the
chain of source locations and the reason, at most eight per function and a
count of the rest. It first reported one chain per function. In l2fwd, the
layer-2 forwarding sample of DPDK (the Data Plane Development Kit), that
chain was a getter defined in another translation unit, and the `printf`
the forwarding loop runs on the main lcore (DPDK's worker thread pinned to
one core) stayed hidden behind it (26 Oct).

Inline assembly is not looked into either, with two exceptions: a string
with no instructions (the compiler barrier `asm volatile("" ::: "memory")`),
and one made only of instructions that cannot wait, each with exactly the
operands it takes -- prefetches (DPDK's `rte_prefetch0` is `prefetcht0` on
x86 and `prfm pldl1keep, [%0]` on arm64), the spin hint (`pause`, also
spelled `rep; nop`, and `yield`), fences and barriers, the time-stamp
counter, and the stack fence: a locked add of an immediate zero to a slot
below `%rsp` (`lock addl $0, -128(%rsp)`), one locked read-modify-write,
with no loop, on a line no other thread holds. Statements end at a newline,
a carriage return or a semicolon, as the assembler's lexer ends them; a
prefix standing alone (`rep;`, `lock;`) is read with the statement after
it; a string holding `%%` (a statement separator on some targets) or a
control character other than those and a tab is not cleared. A bare
instruction may be followed by a `#` comment, which on AArch64 would be an
immediate, and only `isb` takes one there, still only as a barrier. A
lock-free fast path is made of these: in a release build DPDK's ring
enqueue and dequeue reach nothing blocking but that fence, and each of them
failed the check on it alone (26 Oct).

That is a necessary condition for lock-freedom, not a proof of it. A loop
that spins on an atomic until another thread sets it is not judged: it
reaches nothing blocking and is still not lock-free. Proving progress of a
loop is a different analysis, and the bounded waiting the project's
proposal describes is where it belongs.

## Options not taken

- **Report every blocking path.** More complete, and noisier than an error
  should be: below each blocking site only the first chain is followed.
- **Trust functions without a body.** Most are harmless, and the ones that
  are not are the ones that matter. Only a short list that touches nothing
  but its arguments' memory (`memcpy`, `memset`, `strlen` and the like) is
  trusted.
