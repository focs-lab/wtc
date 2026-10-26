# 4. What "lock-free" means when the compiler checks it

Taken 2026-10-19.

Revised 2026-10-26: one chain per blocking site instead of one per
function, and inline assembly that cannot wait is cleared (both under
"What the check establishes"); the table follows the elements held by
value in every implementation since 24 Oct; the facts are shared with the
batching passes ("The same facts for batching").

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
| push | lock-free; allocates a new segment when one fills | wait-free; allocates a new block when one fills | wait-free; allocates a new block when one fills | lock-free; allocates a new segment when one fills | wait-free; allocates a new segment when one fills |
| pop | blocks: waits for a push that has taken its cell; frees drained segments | wait-free | wait-free; lends the element, allocates nothing | wait-free: past a cell a push has taken and not filled, O(h²) in the h cells it has passed and not taken, otherwise O(1) amortised; frees the segments it moves past, and allocates when it has passed more than 64 such cells | lock-free: its claim fails only to another consumer's; frees the segments it moves past |
| lock, unlock | blocks | | | | |

The generic and mpsc columns are as of 22 Oct 2026, when both became the
segmented queue (`runtime/segment_queue.hpp`); until then the general-purpose
queue blocked on its mutexes on both sides and the mpsc list's push was one
exchange. A push to either retries when a pop skipped its cell or another
push is linking the next segment, hence lock-free rather than wait-free.
The mpsc pop never waits, but past a cell a push has taken and not yet
filled it notes every cell it passes and does not take (h of them, filled
or not: one batch still being filled can leave as many as its length, so
a poll behind it costs O(batch)) and looks at them up to h times, while
each look finds an earlier full one; the walk ahead and the step over
cells it has taken out of order pass each cell once (since 26 Oct 2026,
`decisions/0003`). The list of noted cells has room for 64 and allocates
past that, which the progress check reports. A producer that never fills
its cell keeps the consumer's head there for ever, and the segments after
it are then never freed.

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

## The same facts for batching

The batching passes (`--wtc-batch-pushes`, `--wtc-batch-locks`) ask the
same question of the code in a loop they would batch: may it wait for
another thread? Each used to answer it with its own list of functions
and its own walk, and the answers had drifted: both refused every
indirect call the progress check could follow and every inline assembly
string, the lock batching took `realloc` for a wait where the progress
check knew it as the allocator, and the push batching accepted any
function without a body here that was declared nothrow, which says
nothing about waiting (`semop`, a read from a pipe, a join). The facts
now live in one place (`mlir/include/WTC/Transforms/Blocking.h`), and
each pass says only which kinds it accepts: the progress check no kind
of blocking, the lock batching the allocator, the push batching the
allocator and sleeping. Input and output is refused by both: a write to
a pipe or a socket waits for its reader, which may be a consumer of the
queue.

The batching passes add two refusals of their own. One is about
synchronising, not waiting: any atomic or volatile access, fence, or
inline assembly other than prefetches, no-ops and the time-stamp counter
(`rdtsc`, `rdtscp`), or any assembly with a memory clobber, since a loop
that synchronises may be waiting for an element it has not published.
The other is a function the loop calls that acts on a queue or a mutex.

A body is followed only where it is final. A weak definition, a non-ODR
linkonce one, a common or an extern_weak symbol may be replaced at link
time by another translation unit's, which may do anything, and counts as
no body; an ODR inline or template body, an available_externally one and
an internal one are followed. An indirect call is resolved through a
pointer parameter only in a function with internal linkage: an external,
inline or template function can be called from another translation unit
with any other pointer, so its calls seen here are not all its calls.

The allocator is the C library's (`malloc` and its family, `realloc`,
`free`), DPDK's heap, and the replaceable global `operator new` and
`operator delete`, matched by their exact mangled names: plain, array,
aligned, sized and nothrow. A class's `operator new`, or a placement
form taking the program's own arguments (a pool), is the program's code
and may take any lock; without a body here it is opaque.

Accepting the allocator assumes it is a leaf: it waits for no thread of
the program and runs none of its code. A program can register code for
it to run -- a DPDK memory event callback or allocation validator, run
inside `rte_malloc` and `rte_free`; a new-handler, run inside `operator
new` when memory runs short; one of glibc's old allocation hooks -- and
a module that does is given no allocator by either pass, and no batched
pushes at all, since a push allocates when its queue grows. What this
does not see, and assumes away: a registration made in another
translation unit (`set_new_handler` in `main.cpp` while the loop is in
`worker.cpp`); a replaced global `operator new`, an allocator interposed
with `LD_PRELOAD`, and the hooks of jemalloc or tcmalloc, all taken for
leaves; and DPDK's own memory callbacks (VFIO, the mlx5 driver), taken
to hold only locks internal to the library.

Batching pushes costs latency, which is why only a function marked
`WTC_BATCH` is touched: an element waits for up to `WTC_BATCH_K - 1`
later pushes before a consumer can see it, that is, for up to
`WTC_BATCH_K` times the work the loop does between two pushes, sleeps
included. A sleep waits for no thread, so accepting it cannot deadlock:
the batch is published every `WTC_BATCH_K` pushes and at the end of the
loop whatever the consumer does. It can change behaviour: a consumer
that takes a pop timing out for "the producer is gone" may now see that
sooner. In practice the C library's sleeps are declared without nothrow,
and a call to them is refused as one that may throw; a sleep is accepted
only when declared nothrow.

What changed (26 Oct), in verdicts:

- progress check: a call through a pointer parameter of a function with
  external linkage is counted as an indirect call whose target is not
  known, where its targets in this module were followed before; a call
  to a weak or non-ODR linkonce function counts as blocking, where its
  body was followed before;
- push batching, accepted now: a call through a pointer whose targets
  are all known and fit; prefetches, no-ops and the time-stamp counter
  without a memory clobber; the destructor of the shared_ptr of a
  borrowed pop from an inline queue, which owns nothing (a pop is inline
  only after specialisation, which wtc-cc runs after batching, so this
  applies only to IR batched later);
- push batching, refused now: any function without a body here that is
  not the allocator, sleeping, or one that only touches its arguments,
  nothrow or not (in the ClangIR in use, calls into the C library are
  not marked nothrow, so most were refused before as calls that may
  throw; the change matters for functions declared `noexcept`); a weak
  or non-ODR linkonce body; a class or placement `operator new`; a
  static-initialisation guard (in practice the guard's atomic load is
  refused first); any other operation whose name contains `asm`; every
  loop of a module that registers code for the allocator to run;
- lock batching, accepted now: the same resolved indirect calls and
  prefetches; `realloc`, and `rte_free` when declared nothrow;
- lock batching, refused now: `rte_ring_create`, `rte_ring_create_elem`
  and `rte_stack_create`, which take the EAL's tail-queue lock and were
  accepted as the allocator when declared nothrow; a weak or non-ODR
  linkonce body; a class or placement `operator new`; the allocator in a
  module that registers code for it to run; any other operation whose
  name contains `asm`.

And in the progress check's reasons, its verdicts otherwise unchanged: a
call into the threading library that is not a wait (`pthread_create`,
`pthread_self`, keys, attributes) "calls into the threading library";
spin locks, barriers, `pthread_once`, timed and non-blocking joins, C11
`mtx_`, `cnd_` and `thrd_join`, `std::thread::join` and
`std::condition_variable::wait` "wait in the threading library";
`thrd_yield` and `pthread_yield` "sleep or yield"; `semop`,
`semtimedop`, `flock`, `lockf`, `poll`, `ppoll`, `epoll_wait`,
`epoll_pwait`, `select`, `pselect`, the `recv` and `accept` family, the
`wait` family, `pause`, the `sigwait` family, `sigsuspend`,
`rte_eal_wait_lcore`, `rte_eal_mp_wait_lcore` and `rte_thread_join`
"wait for another thread or process"; `__cxa_guard_acquire` "takes a
static-initialisation lock"; `__cxa_throw`, `__cxa_allocate_exception`
and `__cxa_rethrow` "throw"; DPDK's `rte_malloc` family, `rte_free` and
`memalign` "call the allocator"; and DPDK's ring and stack constructors
"take a DPDK library lock". All of these but the guard, `__cxa_throw`
and `__cxa_allocate_exception` ("throws or takes a static-initialisation
lock") were "whose body is not visible here" before. The other way, a
class or placement `operator new` without a body, "calls the allocator"
before, is now "whose body is not visible here".

## Options not taken

- **Report every blocking path.** More complete, and noisier than an error
  should be: below each blocking site only the first chain is followed.
- **Trust functions without a body.** Most are harmless, and the ones that
  are not are the ones that matter. Only a short list that touches nothing
  but its arguments' memory (`memcpy`, `memset`, `strlen` and the like) is
  trusted.
