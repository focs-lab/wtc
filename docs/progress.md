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

## The element's code

A queue operation runs the element's own code: a push its copy or move
constructor, on the producer's side, and a pop its move-assignment (or,
in the shared_ptr form, its move constructor) and the destructor of what
is left in the cell, on the consumer's. The table records which; the
progress check follows each as a call the operation makes. So a consumer
of `wtc::queue<std::string>` that transfers on the spsc ring, whose own
steps neither wait nor allocate, fails `WTC_LOCK_FREE` with the string's
move-assignment in the chain, and a consumer of a trivially copyable
struct passes.

The element type is read from the operation (the pointee of the value a
push is handed, or of the object a transfer moves into), and for a pop
that returns a shared_ptr from the queue's other operations; when it
cannot be read, the operation is counted as blocking. A member is found
by ClangIR's record of special members (`func_info` on `cir.func`).
Assumed: a member that is not in the module is trivial, since the
runtime's template, instantiated for the element, calls every member that
is not and so makes it at least a declaration here.

## Thread counts

A bound in np or nc can be given a number only where the analysis proves
one. The thread-instance walk counts each thread as `Count`: one for main
and the global constructors; for a spawn, the runs of the spawning
function times the trip count of the loops around the spawn; for a call,
the same on the caller's thread. A `for` loop whose induction variable
only its step writes, by one, from a constant, against a constant bound,
runs that many times; against a variable bound it runs as many times as
the variable says, a symbolic count named after it; any other loop, a
goto, a recursion, a launch on every DPDK lcore, and a root standing for
any number of threads (a function whose address is taken, one another
translation unit may call) give no number. A side of a queue is the sum
of its threads' counts, and the ownership analysis leaves it on the
queue's root (`wtc.producers_count`, `wtc.consumers_count`).

Assumed: a symbolic count names the bound as the loop reads it; whether
the variable changes while the loop runs is not judged, which is why a
symbolic count is printed and never instantiated. A loop that leaves
early runs fewer times, so a count is an upper bound; threads started on
two arms of one branch are summed, which overstates.

## Loops

The check used to judge no loop: a spin on an atomic reaches nothing
blocking and is still not lock-free. Each loop of the program's own code
is now classified by what its exit depends on, the backward slice of its
condition and of the conditions (an if's, a switch's) under which it
breaks, returns or continues, through the variables it writes, declared
before it or in it, and the calls it makes
(`mlir/include/WTC/Transforms/ProgressPaths.h`):

| class | exit depends on | level |
|---|---|---|
| counted | an induction variable against a bound the program fixes (a constant read with its signedness, or a variable read without synchronising) | the body's, its bound multiplied, the condition counted once more |
| retry | the loop's own compare-and-swap of a value it read, and nothing else | lock-free; once, on a location no other thread writes |
| wait | an atomic or volatile load, an exchange or fetch-and-op, a compare-and-swap of a constant (a spin lock), one inside a helper of the program's, or one the loop skips while what it read says the word is held | blocking |
| dual | whether a queue had an element, or room | partial: a wait for data |
| unknown | the thread's own data only | wait-free, no bound read |
| forever | nothing: a constant true condition and no break or return | blocking: the path never completes |

A function declared lock-free fails on a wait and on a dual wait. A goto
is read as a loop over its function: a wait if the function reads what
other threads write. The runtime's bodies are not read where a program
uses them (its operations answer through the table), but read directly
the classifier is not more optimistic than the table
(`test/Transforms/Progress/runtime-loops.cpp`).

Assumed, or approximated:

- The program is free of data races. A plain load is no evidence of a
  wait: no other thread writes, while the loop runs, what it reads without
  synchronising. A spin on a plain flag is undefined behaviour, and is
  read as a loop over the thread's own data.
- A call's result depends on everything its callee reads atomically,
  pops or compares-and-swaps; that is coarse, and only ever more
  pessimistic.
- A loop that tries a compare-and-swap of a constant on each element of a
  list (the hazard records' scan) is read as a spin lock, a loop bounded
  by a counter beside an atomic condition (`i < 64 && !flag.load()`) as a
  wait, and a retry whose compare-and-swap is in a helper of the
  program's as a spin lock: pessimistic, by design.
- A retry is lock-free only when its exit depends on its compare-and-swap
  alone: nothing it read decides whether it continues, skips the
  compare-and-swap or leaves. What its body calls is judged on its own.
- A summary read while a function of its call cycle is still being read
  is not remembered, so the answer does not depend on the order in which
  functions are asked about.

## Composing a path

For each function with a requirement, the check composes what its paths
guarantee (`PathComposer`, `ProgressPaths.h`) and reports it in a remark
beside its verdict:

    wtc: take_four(int*): wait-free, at most 652 steps (268 + 384 slip pauses)
    wtc: take_in(int&): wait-free, at most 49·n² + 70·n + 148301 + 68 slip
         pauses steps; allocates (may); on first use, a per-thread record;
         with np = 3 for @in

In sequence the level is the weaker and the steps add; over the arms of
an if, a ternary or a switch the level is the weakest and the steps the
larger (termwise, which bounds both, and summed over a switch's cases,
which may fall through); a loop is taken by its class; a call is its
callee's composition, once per function, and a recursion's depth is not
read. A counted loop's condition runs once more than its body. A composition
made while a function of its call cycle is still being composed is not
remembered. A queue operation contributes its row under its object's counts,
with each count that has a number put in, and the element's special
members as calls. A note names the weakest step: what made the level what
it is, or the bound not a polynomial.

The steps counted are the synchronising ones: the queue operations by
their rows, and one for each atomic or volatile access and each fence or
non-blocking inline assembly of the program's own code. Plain
computation takes none, so a bound says nothing of the time spent
between those steps. Where several objects with different counts meet on
one path, np stands for the largest of them.

## Requirements

| annotation (`shim/progress.hpp`) | the function is held to |
|---|---|
| `WTC_LOCK_FREE` | nothing it reaches blocks (decisions/0004), first-use effects included, and no loop waits or runs without end |
| `WTC_WAIT_FREE` | that, and the composed level is wait-free: no retry another thread can make fail, no operation only lock-free under the counts proved, no wait for data |
| `WTC_WAIT_FREE_WITHIN(k)` | that, and the worst case is a number of steps, at most k |
| `WTC_PROGRESS_CRITICAL` | wait-free, a worst case that is a polynomial in the counts (an amortised bound is not enough), no first-use effect |

ClangIR does not carry an annotation's arguments, so the bound of
`WTC_WAIT_FREE_WITHIN(k)` is part of the annotation's name and must be a
decimal literal. A kernel entry (a futex wake after a publish) is
reported in the remark and fails none of these: it waits for no thread
of the program.

### Requirements the program states without saying so

- **A signal handler** interrupts its own thread anywhere, so what it
  reaches must be async-signal-safe: lock-free, no allocation, no lock, no
  static-initialisation guard, no per-thread first use. A handler is a
  constant function installed by `signal()`, or stored into the
  `sa_handler` or `sa_sigaction` of the `struct sigaction` a call of
  `sigaction()` is handed. The check reports the first chain to what
  blocks: the allocator (`backtrace` included, which loads the unwinder),
  a mutex, a condition variable's wait or notify. A handler that pushes to
  a queue is one more producer (it is address-taken, so the topology
  already counts it as any number of threads), and a push it interrupts on
  its own thread may hold a cell taken and not filled: only the
  single-consumer queues' consumer (mpsc, jiffy) and the rings of indices
  (scq, wcq, whose push publishes its index only once its cell is filled)
  look past such a cell, so a handler's push to any other queue, the
  bounded and settled rings included, is reported.
- **A real-time thread** (`SCHED_FIFO` or `SCHED_RR`, set by
  `pthread_setschedparam` on `pthread_self()` or on a thread whose id
  `pthread_create` wrote, by `sched_setscheduler(0, ...)`, or by
  `pthread_attr_setschedpolicy` with `PTHREAD_EXPLICIT_SCHED` on the
  attributes `pthread_create` is handed) never yields to a thread of
  lower priority on its processor. A thread that raises its own priority,
  directly or through a helper, is at real-time priority from its start
  routine on (pthread_create's and the shim's spawn entries, main, and
  any function nothing here calls, such as a std::thread's `_M_run`); a
  thread it starts with the default attributes, or without explicit
  scheduling, inherits the policy and is checked as one of its own (a
  std::thread it starts is reported as not followed). Its entry must be
  wait-free apart from waits for data, and a wait for data must park and
  take nothing on first use: a `pop_wait` on a queue whose policy spins
  (the default) is refused, and so is a loop polling a queue; a lifted
  wait is allowed where its waiter parks after its pause, that is where
  every `wtc::lifted_wait::construct` of the condition variable it lives
  in (a field by record and index, a global by name) passes paced as a
  constant true, which the wait lifting does for a waiter a requirement
  reaches; otherwise its step is refused, since the policy may spin and
  then yield. What a lifted wait contains is still checked.

  The attributes `pthread_create` is handed are followed through a
  helper's parameter to what every call of the helper passes for it (a
  few calls deep), so a helper that sets explicit scheduling and the
  policy on its caller's attributes makes the thread real-time, as does
  one that sets only the policy where main set explicit scheduling.
  Attributes that cannot be followed to every thread they start (a
  helper with external linkage, which another translation unit may call,
  or one nothing here calls) are reported as not checked. A policy that
  is not a constant here is not taken for a requirement (decided 28 Nov):
  read as real-time, a policy from a configuration set on main made
  everything main reaches, and every thread it starts, wait-free-required,
  which refused batching and delegation and chose slower queues across
  the program. It is reported instead, and a program that means it states
  the requirement on the thread's entry (`WTC_WAIT_FREE`).

These requirements are the program's, unstated: their violations are
warnings, or errors under `--wtc-check-progress=inferred-errors=true`. A
handler or a thread the analysis cannot name (a handler that is not a
constant, a policy set on a `std::thread`'s native handle) is reported
as not checked.

Every requirement that reaches a function counts, not only the first one
found: the passes that respect requirements take the strictest
(`RequirementRoots::allReaching`, and `reaching` with a predicate).

Assumed: a policy that is not a constant is not real-time (it is
reported); the requirement of a thread that raises its own priority holds
from its start routine; the Linux values of `SCHED_FIFO`, `SCHED_RR` and
`PTHREAD_EXPLICIT_SCHED`; a handler that runs on a thread which blocks
its signal is still checked.

## Choosing for a requirement

The specialisation pass gains one input: the requirements on the paths
that reach a queue's operations (`RequirementRoots::allReaching`). An
implementation meets them when every operation of the queue on a
requiring path passes, under every requirement that reaches it and the
counts proved, what the progress check holds that operation to on its
own: the level (lock-free, or wait-free for a declared wait-free
requirement and a real-time thread), no wait for data (but a real-time
thread's), no allocation, no first-use effect, a worst case of at most k
steps for `WTC_WAIT_FREE_WITHIN(k)` and a polynomial one for
`WTC_PROGRESS_CRITICAL`, and for a signal handler's push a consumer that
looks past a cell a push holds. Every operation counts, not only the one
that asked: the consumer of a queue whose producer is the requiring path
keeps its own requirement. These criteria are necessary, per
operation: the pass does not compose the requiring path (k bounds one
operation's worst case, not the path's sum of steps) and does not read a
queue's wait policy for a real-time thread's wait for data, so the
progress check, which does both, can still fail the build after a tag is
rewritten. When the implementation chosen for the topology does not meet
them, the pass picks the first of its candidates that does, in order of
cost:

| queue | candidates, in order | what each needs |
|---|---|---|
| unbounded, one consumer | jiffy | none chosen today: every push of an unbounded queue may allocate, and jiffy's takes a per-thread record on first use |
| unbounded, many consumers | none: the remark suggests a capacity | |
| bounded, one producer and one consumer | spsc_settled, scq, wcq | the settled ring nothing; the rings of indices their thread bound |
| bounded, many producers, one consumer | mpsc_settled (its push), scq, wcq | as above |
| bounded, one producer, many consumers | spmc_settled (its pop), scq, wcq | as above |
| bounded, many on both sides | scq, wcq | as above |
| bounded, built to overrun or taking batches | none: the ring is kept whatever the tag | |

Since every requirement refuses the allocator and first-use effects, as
the check does, jiffy (whose push may allocate and takes a per-thread
record) and wcq (whose first operation takes a hazard record and a
thread slot) meet none today: the remark names them with the reason.
What is chosen is a settled ring or scq. A queue built by more than one
constructor call is not specialised for a requirement, and a remark says
so.

The settled ring comes first: it costs less throughput than the rings of
indices (its spsc and spmc forms ran at 0.62 and 0.73 of the plain ring's,
`bounded_ring.hpp`; the queues over the SCQ and wCQ rings at 0.24 to 0.78
and 0.06 to 0.79 of it, `runtime/README.md`) and needs no bound on
threads. It suffices only for its single side and for a many side asked
to be lock-free; a single side's wait for a many side remains, so an
mpsc consumer with a requirement gets a ring of indices.

A ring of indices is chosen only when its count is shown within the
bound its marker states (the shim's 128): the producers and consumers
proved, summed, must be a number (a thread on both sides counts twice,
which overstates), and no operation of the queue may run the element's
own code, since an operation that code starts on the same queue is one
more in flight and the bound counts it apart. A count with no number, a
symbolic one included, is not shown within any bound, and the remark
says so with the count.

An implementation is chosen only when the runtime the program is built
with announces it: the queue's constructor calls a marker annotated
`wtc_queue_impl_<name>` with the tag that selects it, and for a ring of
indices its bound on threads, so the tag written is the runtime's own
number. The shim announces jiffy on the unbounded queue, and the settled
rings and scq on the bounded one; wcq only where the build's
double-width compare-and-swap is one instruction (`-mcx16` on x86-64).
ClangIR cannot yet compile that builtin (`__sync_bool_compare_and_swap_16`
is not implemented in its code generation), so a wtc-cc build never
announces wcq. Every choice that fails names the requirement and, for
the current implementation and each candidate, the first operation it
fails and why.
`test/Transforms/Specialize/requirements.cpp` shows each choice against
queue types that announce them, and
`test/Integration/Lifted/requirement_bounded.cpp` the choices of
spsc_settled, mpsc_settled and scq on the shim's runtime, built and run,
with the remarks where nothing meets the requirement. A program's own tag
is left alone, as before.

## What a requirement refuses

- **Delegation** (`--wtc-delegate-sections`): a section reached from a
  path with any requirement (a declared one, a handler, a real-time
  thread) is not delegated, with the chain in the refusal: the client
  would wait for the server thread, which runs other clients' sections
  first, at the priority of the client that started it. No section of a
  mutex that may use priority inheritance is delegated: a protocol other
  than `PTHREAD_PRIO_NONE` (or one that is not a constant) set on any
  attributes object that may reach the mutex's `pthread_mutex_init`.
- **Batching pushes** (`--wtc-batch-pushes`): not when a consumer of a
  queue of the same type is progress-critical, declared wait-free within
  k steps, or on a real-time thread, whichever other requirements reach it
  too. The pass runs before queues are told apart, hence the type.
- **Batching locks** (`--wtc-batch-locks`): not when a thread with a
  requirement takes the same mutex, or one that may be the same, the
  loop's own lock included: another thread may run the same loop under a
  requirement. Mutexes are told apart by what they are: a global by its
  name, a member by its record and field (a mutex is held by value, so
  members of different records or fields are different objects); one
  reached through a pointer may be any.
- **Lifting waits** (`--wtc-lift-locked-queues`, docs/lifting.md "Lifting a
  wait"): the lifted wait's policy may spin and then yield before it
  parks, chosen at run time from the counts, which a waiter at real-time
  priority must not do (a yield keeps a producer of lower priority off
  its processor; the runtime's own check of the scheduling policy reads
  the thread that builds the class, not the waiter). A wait whose
  function a real-time thread or a declared requirement reaches is lifted
  with the policy that parks after its pause, as a paced producer's is,
  and the remark says so with the chain. That keeps the original's
  guarantee: the condition variable's waiter sleeps, and so does this one,
  after a bounded pause. A wait a signal handler reaches is not lifted:
  the original's wait was already the handler's to answer for, and the
  eventcount's state, which a handler could reenter on the thread it
  interrupted inside the same wait, is not proven safe for that, so
  keeping the original keeps whatever the program had. The progress check
  refuses a spinning `pop_wait` on a real-time thread in the same spirit.
