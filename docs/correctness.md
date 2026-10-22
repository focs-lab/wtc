# Why substituting a queue preserves the program

A sketch of the correctness argument, in the form a paper's section would
take: what the analyses prove, what each implementation promises under
those facts, and why a program whose queues the compiler has changed does
everything the original did and nothing else. The contract is
`decisions/0003`; progress is `decisions/0004`; the run-time choice is
`decisions/0005`; the invariants of each implementation are stated at the
top of its header under `runtime/`; the shim that carries the tag is
`shim/queue.hpp`. Nothing below adds a promise those files do not make.

## 1. The objects and their operations

A queue object `q` of type `wtc::queue<T>` is built once, with a tag, and
destroyed once. Between the two, threads call `push` (copy or move an
element in), `try_pop()` (the oldest element, or null, as a `shared_ptr`),
`try_pop(T&)` (the oldest element moved into the caller's object, or
false), and the batch operations, which are a `push` run published
together. The tag selects an implementation when the object is built and
never changes afterwards (`shim/topology.hpp`).

The original program uses the general-purpose implementation for every
object. The substituted program uses, for some objects, one of `spsc`,
`spsc_inline`, `mpsc` or `spmc`, or a tag computed at construction from a
loop bound (`decisions/0005`). Everything else is the same code.

**Definition 1 (observable behaviour).** The sequence of values each
thread obtains from its pops, the answers "empty" it receives, the values
every other operation of the program computes, and the happens-before
relation among the program's own accesses to memory, as the C++ memory
model defines it, restricted to operations other than the queue's internal
ones.

**Definition 2 (the contract).** An implementation is a *contract queue*
for an object if, in every execution satisfying the implementation's
precondition: (C1) every element pushed is popped at most once and, if the
object is drained, exactly once; (C2) the operations on the object are
linearisable with respect to a sequential FIFO queue whose `try_pop` on an
empty queue returns empty; (C3) elements pushed by one thread are popped in
the order that thread pushed them; (C4) the push of an element
happens-before the pop that returns it; (C5) nothing is ordered against
other objects. This is the contract of `decisions/0003`, option 2.

## 2. The facts the analyses prove

**Fact T (topology).** For each queue object, the thread-instance walk
(`mlir/lib/WTC/Transforms/ThreadInstances.cpp`) computes an
over-approximation of the set of threads that may execute each push and
each pop of that object over the object's lifetime, as a count on each
side: one or many. "One" means every execution of the program has at most
one thread calling that side of that object. The walk counts a spawn in a
loop as many, a function whose address is taken as callable by any thread
any number of times, an unresolved entry as many, a function called from
no known place as many; a counted spawn loop records the loop instead of
a count (section 5). The destructor is ordered after every operation: a
queue is destroyed by its owner after the threads that use it have been
joined, or it is a global destroyed after `main`, and the analysis does
not place a verdict on an object whose destruction it cannot order after
its uses.

**Fact O (ownership).** For a queue proved single-consumer, the borrow
analysis (`WTCBorrowAnalysis.cpp`) proves, for every pop of the object,
one of: (O-borrow) the program uses the returned `shared_ptr` only through
`operator bool`, `get`, `*`, `->`, comparison with null and the
destructor, keeps no pointer into the element past its next pop of the same
object, and calls nothing in that scope that may pop the object;
(O-transfer) the pop is `try_pop(T&)`, so the element is moved out and
nothing of the queue's is kept; (O-share) neither, in which case the
returned `shared_ptr` owns a copy of the element. `spsc_inline` is chosen
only when every pop is O-borrow or O-transfer and `T` is trivially
destructible.

**Fact R (run-time count).** Where a side's threads are all started by
one counted loop `for (i = c; i < n; ++i)` that spawns one thread per
iteration unconditionally, in a function that runs once, with the queue
built in that function before the loop and every store to `n` before the
construction, the number of threads on that side equals the value of `n`
read at the construction (`decisions/0005`).

## 3. Each implementation is a contract queue under its precondition

**Lemma 1 (spsc ring).** `spsc_ring<T>` is a contract queue whenever
exactly one thread ever pushes and exactly one ever pops.

*Sketch.* Elements are numbered by the producer; element `i` lives in a
fixed slot of a block (`runtime/spsc_ring.hpp`, "Design"). The producer
writes the slot, then publishes `push_pub_ = i + 1` with a release store;
the consumer reads `push_pub_` with an acquire load and reads slot `i` only
when `i < push_pub_`. The pair gives C4 for every element, and since the
producer writes slots in order and the consumer reads them in order, C1
and C3 hold. C2: take the linearisation point of a push at its release
store and of a pop at the acquire load that first observed its element
(or, for an empty answer, at the acquire load that returned the current
count); the single consumer reads elements in index order, so the
sequential queue matches. The slip (the consumer waiting, boundedly, for a
batch) delays a pop's linearisation point but never moves it before the
element's publication and never answers empty while an element is
published: the empty answer is given only when the acquire load returned a
count equal to the consumer's own. Block reuse: the consumer announces that
it has left a block (`pop_pub_`, release) only after loading the block's
`next` pointer, and the producer reuses or inserts a block only after
acquiring `pop_pub_` past the block's end, so no slot is overwritten while
the consumer can still read it. The precondition is what makes the two
private states (`p_`, `c_`) single-writer; a second thread on either side
races on them, and nothing in the ring is defined after that.

**Lemma 2 (spsc ring, inline).** With `spsc_inline`, `try_pop()` returns
a `shared_ptr` that owns nothing and points into the slot; the element is
destroyed in place at the consumer's next pop or at the queue's
destruction. Under O-borrow the program's observable behaviour is that of
Lemma 1's queue returning a copy.

*Sketch.* By O-borrow the program reads the element only between the pop
and its next pop of the same object, and keeps no pointer past it; in that
interval the slot is not overwritten (Lemma 1's reuse rule, since the
consumer has not announced leaving the block) and not destroyed (the ring
destroys the lent element only at the next pop). Every read the program
makes therefore returns the value the producer wrote, which is the value
the copy would have held. The difference that remains, when the element's
destructor runs, is unobservable because `T` is trivially destructible.
Under O-transfer the element is moved out, as with any implementation.

**Lemma 3 (spmc ring).** `spmc_ring<T>` is a contract queue whenever
exactly one thread ever pushes.

*Sketch.* The producer's side is Lemma 1's. Consumers take the ring's
`try_pop` one at a time under a spin lock (`runtime/spmc_ring.hpp`): the
lock's acquire/release makes the consumer side sequential, so Lemma 1's
argument applies to the sequence of pops in lock order, with each pop's
linearisation point inside its critical section; the slot is moved out and
released before the lock is let go, so the ring's reuse rule sees one
consumer. The element is never lent (the shim does not choose
`spsc_inline` here), so there is no Lemma 2 obligation.

**Lemma 4 (segmented queue, both forms).** `segment_queue<T, many>` is a
contract queue with no precondition beyond the destructor's ordering;
`segment_queue<T, one>` is a contract queue whenever exactly one thread
ever pops.

*Sketch.* Cells are taken by fetch-and-add on the segment's `enq` and
`deq` counters (`runtime/segment_queue.hpp`, "Design"); a push writes its
element then marks the cell full with a release (store or compare-exchange),
a pop reads the mark with an acquire before the element, which is C4. C2:
linearise a push at its mark and a pop at its fetch-and-add on `deq`;
cells are consumed in index order per segment and segments in list order,
so the sequential order is the cell order. A multi-consumer pop that finds
its cell unreserved waits briefly and then marks it skipped, and the push
that later takes that index moves on, so an element never lands in a cell
that has been passed; a pop that finds its cell reserved waits for its
mark, so a batch is never torn (C3 for batches). The single-consumer form
takes no fetch-and-add on `deq` and returns empty at an unfilled cell;
"empty" is then answered when the only consumer's next cell is not yet
full, which the contract allows ("a pop that reports empty does so at its
linearisation point"; `decisions/0003`, update of 22 Oct). Segments are
freed through hazard pointers (`runtime/hazard.hpp`): a thread protects
the segment it reads with a sequentially consistent store and a reload,
and a segment is retired only after the tail and the head have moved past
it, so no thread frees a segment another may still read. The elements
themselves are moved into cells and out of them, so the element's memory
never crosses threads except as the queue's own.

**Lemma 5 (the general-purpose queue is a contract queue).** Immediate from
Lemma 4 with `many`.

## 4. The substitution preserves the program

**Theorem 1.** Let `P` be the program as written and `P'` the program with
some objects' tags changed by the compiler. If every changed object's new
implementation has its precondition satisfied in every execution of `P'`,
then every observable behaviour of `P'` is an observable behaviour of `P`.

*Sketch.* The programs differ only in the implementation behind each
changed object's operations, and every implementation is a contract queue
under its precondition (Lemmas 1 to 5), as is the general-purpose queue
without one (Lemma 5). Take an execution of `P'`. For each changed object,
C2 gives a linearisation of its operations consistent with a sequential
FIFO queue; replacing the implementation's internal steps by those of the
general-purpose queue with the same linearisation yields an execution in
which every pop returns the same value or the same empty answer (C1 to
C3), and in which every push still happens-before the pop of its element
(C4). The program's own accesses are unchanged, and no ordering between
objects was relied on in `P` beyond what C5 withholds from every
implementation including the general-purpose one, so the happens-before
relation restricted to the program's accesses is preserved. Hence the
execution of `P'` is, in its observable behaviour, an execution of `P`.

The proof needs the precondition, which is where the analyses come in.

**Lemma 6 (the precondition holds).** If Fact T gives one thread on a side
of an object, every execution of `P'` has at most one thread on that side;
if Fact O gives O-borrow or O-transfer for every pop, the obligations of
Lemma 2 hold.

*Sketch.* Fact T is an over-approximation of the set of threads reaching
each operation, computed on the whole translation unit under the
assumptions of section 6; the implementations' preconditions ask for
nothing finer than "one thread on this side", and the tag rewrite is
applied only where the walk gave "one" on every side the chosen
implementation requires (`WTCSpecialize.cpp`). Fact O is checked on every
pop the ownership analysis attributes to the object, including pops in
helpers called for several objects, each of which is checked for each
(`decisions/0003`'s R11 note in `docs/evaluation.md`).

## 5. The tag chosen when the queue is built

**Lemma 7.** Under Fact R, the implementation selected at construction
(`spsc` or `spsc_inline` when the bound is at most one, else the static
verdict's) has its precondition satisfied.

*Sketch.* The bound `n` is loaded where the queue is built; every store to
`n` in the function precedes that load and `n`'s address goes nowhere
else, so the loop reads the same value; the loop spawns exactly `n`
threads on that side, unconditionally, once (the function runs once);
hence the side has at most one thread exactly when the load gave at most
one. The select is a plain value computed before any thread of that side
exists, so the chosen implementation is fixed before the first operation.
The object's every operation records the general-purpose implementation
for the purposes of later checks (`decisions/0005`), which is the
pessimistic answer and sound for them.

## 6. What the proof assumes of the analysis, and what breaks where

A reviewer should see each soundness obligation and the lemma it serves.

- **Thread instances over-approximate.** Any thread that may reach an
  operation is counted. The walk treats a spawn in a loop, a spawn with an
  unresolved entry, a function whose address is taken, a function called
  from nowhere visible, and recursion as "many" (`ThreadInstances.cpp`).
  A missed thread breaks Lemma 6 and so the precondition of Lemmas 1 to 3
  and the single-consumer form of Lemma 4; the general-purpose queue is
  immune. The `std::thread` entry is read from the state object the
  constructor builds, and `_M_run`'s address in the vtable is exempt from
  the address-taken rule only because its one caller is the library's
  native start, which the walk models.
- **One translation unit.** A function called from another unit is a root
  the walk cannot see; the analysis counts it as many, which is sound and
  what dedup's `Encode` shows (`benchmarks/apps/dedup`).
- **Objects the walk cannot place.** A queue member reached through a
  pointer the walk cannot follow is keyed by its record type and offset,
  and its verdict is the union of the threads of every object the one
  constructor call builds; the tag is rewritten on that constructor, so it
  applies to all of them, and the union covers each. Precision is lost
  (two single-thread objects give many), soundness is not.
- **Ownership is checked per pop, syntactically.** A pointer that leaves
  the scope through a path the checker does not follow (a store the
  checker refuses, a call without a body) makes the pop not borrowed, which
  is the safe side; a pointer that leaves through a path the checker
  wrongly accepts would break Lemma 2.
- **The destructor is ordered after the uses.** The walk relies on the
  program's joins; a queue destroyed while a thread may still use it is a
  bug of the original program under any implementation, and not covered.
- **The counted loop's shape.** Lemma 7 holds only for the shape
  `decisions/0005` describes; any other shape keeps the static verdict.

## 7. What is not claimed

- **Progress.** The substitution does not preserve progress properties in
  the strict sense and does not claim to: every implementation allocates
  when it grows, the general-purpose pop waits for a reserved cell, the
  spmc pop waits for another consumer's. `decisions/0004` answers, per
  operation, what each implementation can wait for, and the lock-free check
  reports it; what is claimed is that a substitution never introduces a
  wait the general-purpose queue would not also have had.
- **The element's destructor** runs at a different time under
  `spsc_inline`; the shim restricts that tag to trivially destructible
  elements, so the difference is unobservable.
- **Ordering across objects** (C5) is promised by no implementation,
  including the original mutex queue once the general-purpose
  implementation became the segmented queue (22 Oct 2026); a program that
  relied on the mutex's incidental total order is outside the contract as
  written.
- **The transfer form** has no observable difference from the shared form
  beyond the absence of an allocation; it is the program's own object that
  receives the element.
- **Timing.** The slip in the ring and in the single-consumer segmented
  queue delays pops by a bounded number of pauses; the contract has no
  timing clause.
