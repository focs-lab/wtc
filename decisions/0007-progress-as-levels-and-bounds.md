# 7. Progress as levels and bounds

Taken 2026-11-24. Supersedes the table of decisions/0004 ("What an
operation guarantees"), its jiffy column included; 0004's account of what
the lock-free check establishes, of inline assembly and of the shared
facts for batching stands.

Revised 2026-11-28: the table is read against the runtime at 31ab3e2
(the progress runtime: the tick rate measured before the first pop,
hazard records taken without a TLS destructor, retire and scan without
allocating, the single consumer's step bounded per call, the one-counter
push as jiffy, the settled bounded rings, and the rings of indices), and
the choice for a requirement knows the settled rings and the rings'
bound on threads ("Revision of 28 November", below).

## Context

decisions/0004 gave each operation two answers, can it wait for another
thread and may it allocate, and judged no loop. Read against the runtime
at e9226e1, that table was wrong or optimistic in several places:

1. The spsc pop is wait-free only in steady state: its first catch-up in
   the process reads a function-local static behind
   `__cxa_guard_acquire`, whose initialiser spins for 200 µs, and every
   pop carries a bounded slip of up to 96 pauses.
2. Every operation that uses hazard pointers (generic push and pop, mpsc
   push and pop, spmc pop) registers a thread-local record on its
   thread's first use, which allocates and takes the dynamic linker's
   lock.
3. The mpsc pop is wait-free with a history-dependent worst case: the
   step over cells taken out of order runs inside one call, linear in
   how many there are. Its hole walk is bounded by the pushes in flight.
4. The pops of the segmented queues allocate through retire and scan as
   well as freeing segments.
5. There was no bounded column: every bounded operation blocks, on every
   topology, spsc included.
6. "No producer passes the check because the queues are unbounded" is
   out of date: a bounded producer does not allocate; it blocks.
7. A pop that moves the element out allocates nothing only for a
   trivially movable and destructible element: the element's
   move-assignment and destructor run inside the operation and were not
   followed.
8. The generic push is lock-free also because pops skip its cells, not
   only because a segment is being linked.
9. The futex wake after a publish is a sleeping lock on PREEMPT_RT.
10. Loops were not judged, and items 1 to 7 are what reading the loops by
    hand had missed.

## Decision

Each operation answers with a specification under the thread counts
proved for its object: level (blocking deadlock- or starvation-free,
obstruction-free, lock-free, wait-free), partial (a wait for data),
worst-case and amortised bounds as polynomials in the counts,
allocation, kernel entry, first-use effects, and the element's special
members it runs (docs/progress.md). The table is code
(`mlir/lib/WTC/ProgressTable.cpp`), printed by `--wtc-progress-table` and
held row by row by `test/Dialect/progress-table.mlir`, so prose cannot
drift from it again; 0004's table is not maintained.

Items 1 to 5 and 7 are rows of the table; 7 is also followed by the
check. Item 8 is in the row's evidence. Item 9 is recorded as a kernel
entry and fails no requirement. Item 10 became the loop classifier.

On top of the table: thread counts as numbers where the program fixes
them, loops classified by what their exit depends on, a path's progress
and bound composed and reported, three more promises
(`WTC_WAIT_FREE`, `WTC_WAIT_FREE_WITHIN(k)`, `WTC_PROGRESS_CRITICAL`), the
requirements signal handlers and real-time threads state without
saying so, and the other passes refusing what would break a requirement.

## Options not taken

- **Fix the prose of 0004 row by row.** It would drift again, as items 5
  and 6 did when the bounded queue arrived.
- **Let the runtime's code speak for itself**, by walking its bodies
  where a program uses it. The runtime's loops are written for
  throughput, and the classifier reads several of them more pessimistically
  than they are (the slip, the hazard scan); the table is the reviewed
  statement, and a test checks, for the rows it reads directly (the
  bounded ring, plain and settled, the queue over the SCQ ring, jiffy's
  push, the general-purpose pop and the spsc pop,
  `test/Transforms/Progress/runtime-loops.cpp`), that the classifier is
  not more optimistic than it; the wCQ ring is not read, since ClangIR
  does not compile its double-width compare-and-swap.
- **Errors for inferred requirements.** A handler that is not
  async-signal-safe is a bug, but `--wtc-check-progress` runs in every
  wtc-cc build; warnings by default, errors on request.

## Revision of 28 November

The runtime answered most of the items above, and the rows follow it:

- items 1 and 2: no operation takes a static-initialisation guard (the
  tick rate is measured when the first queue or waiter is built), and the
  per-thread record is taken with one compare-and-swap or fetch-and-add;
  it still sets a pthread key, which calls the allocator unless the key is
  among the C library's first 32 or when more than 256 threads hold
  records, and the check, which cannot know the process's keys, counts it
  as a first-use effect;
- item 3: the mpsc pop's bound holds for every call, its hole looks plus
  one retire and its scan (49·n² + 70·n + 8 steps over the n hazard
  records), a polynomial;
- item 4: retire and scan allocate nothing; the scan still frees;
- jiffy is the runtime's one-counter push: wait-free, with no bound in the
  counts for a push preempted while segments are linked, and a pop whose
  scan walks what a push's mark keeps (both "not bounded in the counts");
  its earlier rows, np + 5 and 4·np² + 4·np + 2, were the design's and are
  gone;
- the settled rings are rows of their own: both sides of the spsc form
  wait-free, the many side lock-free (wait-free at a count of one), the
  single side's wait for a many side kept;
- scq is lock-free; wcq is wait-free only where the producers and
  consumers proved sum to a number within its 128 thread slots, and
  lock-free otherwise, with no closed form and two of six GenMC
  configurations checked to completion (runtime/README.md);
- the bounded producers' rows named the consumer's element members; they
  name the copy or move constructor now.

The choice for a requirement holds each candidate to what the check
holds each operation of the queue to, under every requirement that
reaches it: no allocation and no first-use effect among them, so jiffy
and wcq are chosen for no requirement today. The criteria are necessary
per operation, not the composed path (a bound within k is one
operation's) nor a real-time wait's policy, so the check, which reads
both, can still fail a build after the tag is rewritten. It takes the settled ring
of the queue's topology before the rings of indices, which cost more
throughput and need a bound on threads, and chooses a ring of indices
only when the counts proved are within the bound its marker states and
the element runs no code of its own inside an operation. A queue built
to overrun or to take batches keeps its ring. wcq is announced only with
a double-width compare-and-swap of one instruction, which ClangIR cannot
yet compile.

Also revised 28 November: a scheduling policy that is not a constant is
reported rather than taken for a real-time requirement, and attributes
set up by a helper are followed through its parameter.

## Costs

The rows are read from the runtime's code at one revision and must be
read again when it changes; the table test shows every change, not
whether the runtime made one. Bounds count synchronising steps, not time.
The assumptions are listed in docs/progress.md.
