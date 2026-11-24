# Progress: levels, bounds and requirements

What the compiler knows about the progress of a wtc operation, how it
composes that into the progress of a path, and where the requirements a
path is held to come from. decisions/0004 says what `WTC_LOCK_FREE` means;
this note describes the specification that replaced its two booleans.

## The specification

`ProgressOpInterface::getProgressSpec(counts)` answers, for one operation
under the thread counts the analysis proved for its object
(`mlir/include/WTC/Progress.h`):

| field | values |
|---|---|
| level | `blocking(df)`, `blocking(sf)`, `obstruction_free`, `lock_free`, `wait_free` (the taxonomy of "On the nature of progress", OPODIS 2011) |
| partial | a dual operation, which waits for data ("Nonblocking concurrent data structures with condition synchronization", DISC 2004): `pop_wait` on a wait-free ring is `partial(wait_free)` |
| bound | for a wait-free operation, its own steps in the worst case: a polynomial in np, nc and n (producers, consumers, hazard-domain threads), or *finite, not instantiated*, or *not bounded in the counts* (history-dependent) |
| amortised | the amortised bound, where it differs |
| allocates | never, on first use, once every K operations, may |
| kernel | none, futex wake, futex wait |
| first use | a per-thread record taken by a thread's first operation (the runtime's hazard record, and the wait-free ring's thread slot: its pthread_setspecific may call the allocator), a static-initialisation guard (the program's own: no operation of the runtime takes one) |
| element | which of the element's special members it runs |

A step is one access to shared memory or one pause of a bounded wait.
Steps are not time.

The old answers are read from the specification under no counts: an
operation *blocks* when it is partial or short of lock-free, and *may
call the allocator* when it allocates at all. The progress check and the
batching passes still ask those.

## The table

One table, keyed by form, implementation and counts
(`mlir/lib/WTC/ProgressTable.cpp`), with the evidence for each row in its
comments. `wtc-opt --wtc-progress-table` prints it as Markdown, and
`test/Dialect/progress-table.mlir` holds every row, so a change of any row
is a change of that test. The rows that differ from the two booleans:

- the counts upgrade cells: the mpsc push with one producer is
  wait-free (two tries of push_run, and at a segment's end up to 64 polls
  and pauses for a link only it makes: 80 + 64 pauses), the spmc pop with
  one consumer is wait-free (its claim never fails: 23 + 70·n + 49·n², n
  the threads whose hazard records a retire's scan reads), and so are the
  many side of a settled ring and the wait-free ring of indices where
  their counts allow (below);
- a retire and the scan it may start read the seven slots of each of the
  n hazard records and, for each slot that holds a pointer, the retired
  nodes waiting (at most 8 plus those held at the last scan): 49·n² +
  70·n + 8 steps, allocating nothing (`hazard::scan`), though the scan
  frees what no slot holds;
- the spsc pop's slip polls the producer's count at most 64 times with at
  most 96 pauses between (67 + 96 pauses);
- the mpsc pop is wait-free with a bound for every call: h² hole looks
  and the erase of the hole it takes, its slip, and one retire with its
  scan (since the runtime moves the head past the cells taken out of
  order at once, `decisions/0004`). The walk adds holes only when none is
  full, so the holes are cells reserved by pushes still in flight, plus
  the full cell the walk stops at: a producer has at most five pushes in
  flight (one, and four nested in its elements' code, `hazard.hpp`,
  `kNestedPushes`), each reserving up to 64 cells (a batch's run), so
  h <= 320·np + 1 and h² + h <= 102400·np² + 960·np + 2;
- every operation that uses hazard pointers takes a per-thread record on
  its thread's first operation;
- a push of the segmented queues builds the element into a temporary,
  moves it into its cell and destroys the temporary, and takes it back by
  move-assignment when it loses the cell; a batch's publish moves and
  destroys its elements; an overrunning push destroys the oldest;
- every publish is followed by the waiter's notify, a fence and a load,
  counted;
- every bounded operation is `blocking(sf)` on every topology.

A first-use effect blocks as the program's own would: a function
declared lock-free or wait-free that reaches an operation with one fails,
with the effect as the reason. On the runtime in this tree that is the
per-thread record of every operation of the segmented queues (generic,
mpsc, jiffy), of the spmc pop and of the wait-free ring of indices. A
thread takes it with one compare-and-swap or fetch-and-add, but sets a
pthread key for it, which calls the allocator unless the key is among the
C library's first 32 (`hazard::key_allocates`, known only at run time) or
when more than 256 threads hold records at once; the check cannot tell,
and counts it. No operation takes a static-initialisation guard: the
tick rate the spsc slip and the waiters read is measured when the first
queue or waiter is built (`stream_gate.hpp`, `ensure_ticks_per_ms`), so
the spsc pop and every waiting operation have no first-use effect. Bound
arithmetic saturates: a bound past 64 bits is finite and not
instantiated.

### Implementations chosen for a requirement

Six implementations serve only a path's requirement, since each costs
throughput (`runtime/README.md`); their rows are read from the runtime:

- `spsc_settled`, `mpsc_settled`, `spmc_settled`: the bounded ring whose
  single side fills or empties its cell before it publishes its counter
  (`bounded_ring.hpp`, "The sides"), so that the other side never waits
  for it. Both sides of the spsc form are wait-free (a push 9 steps, a pop
  10, the waiter's notify included); the mpsc push and the spmc pop,
  the many side, are lock-free, wait-free when their count is one; the
  single side's wait for the many side remains, so the mpsc pop and the
  spmc push stay blocking. A queue built to overrun or to take batches
  keeps the plain ring whatever the tag, and so does its row.
- `scq`: the bounded queue over the SCQ ring of indices ("A scalable,
  portable, and memory-efficient lock-free FIFO queue", DISC 2019), its
  exact variant: no operation waits for another thread and nothing
  allocates after construction; lock-free, no step bound. It is built for
  128 threads (`shim/bounded.hpp`, `kIndexThreads`): at most that many
  operations in flight at once, one an element's own code starts on the
  same queue counting apart; past it the queue may abort.
- `wcq`: the same queue over the wCQ ring ("wCQ: a fast wait-free queue
  with bounded memory usage", SPAA 2022), wait-free under two conditions:
  the double-width compare-and-swap is one instruction (the shim announces
  wcq only then), and every thread operating on it holds one of its 128
  thread slots, which holds while at most 128 threads that have operated
  on it are alive at once. A thread past the slots runs SCQ's loops, and
  while one does the others are not wait-free either, so the row is
  wait-free only where the producers and consumers proved sum to a number
  within 128, and lock-free otherwise. The paper proves the steps finite
  and gives no closed form; none is claimed (the runtime measured 25 to
  150 steps for the slowest operation of the others with one thread
  frozen, `runtime/README.md`). Two threads take no slot and run SCQ's
  loops, whatever the count: one whose hazard record could not be tied
  to its exit (the pthread key's allocation failed, until it succeeds),
  and an operation from a key destructor that runs after its thread's
  exit; the row's note states both, and the counts cannot exclude them.
  What is model-checked is narrow: GenMC
  checks the wCQ ring to completion in two configurations only, on a ring
  of two indices with 32-bit halves, an enqueue a dequeue then sees and two
  enqueues on the slow path helping each other; the four with a dequeue on
  the slow path or more operations do not complete, and the slow-path
  dequeue is tested, not model-checked (`runtime/README.md`, "What is
  model-checked"). A thread's first operation takes a hazard record and a
  thread slot.
- `jiffy`: the single-consumer segmented queue with the push that takes
  its index from one counter for the whole queue, as in "Jiffy: a fast,
  memory efficient, wait-free multi-producers single-consumer queue"
  (`segment_queue.hpp`, `kOneCounter`). Its push is wait-free: one
  fetch-and-add, at most five reads of the tail and one store and read
  each time it protects it, and the segments between the tail and its
  index, which the pushes in flight put there when it is not preempted,
  and which are those linked meanwhile when it is; no bound in the counts
  is claimed for the latter, so its bound is "not bounded in the counts".
  Its pop is the mpsc pop, whose retire's scan also walks, while a push
  holds its queue's mark, the segments retired since the oldest such mark
  (up to the queue's backlog when that push took its index): its worst
  case is likewise not bounded in the counts, and the mpsc pop's bound is
  its amortised one.

