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

**Lemma 3 (spmc queue).** `spmc_queue<T>` is a contract queue whenever
exactly one thread ever pushes.

*Sketch.* The producer alone constructs elements in the tail segment's
cells, in order, and publishes a prefix of them with a release store of the
segment's count `enq`; a full segment is published whole before the next
is linked (`runtime/spmc_queue.hpp`). A pop reads `enq` (or the consumers'
copy `seen`, stored with a release by a consumer that read `enq` with an
acquire) with an acquire and claims the cell at `deq` by a compare-and-swap
from `d` to `d + 1` only when `d` is below what it read, so `deq` never
passes `enq`, every claimed cell is published, and no cell is claimed
twice. C4: the element's construction happens-before the release of `enq`,
which happens-before the claiming pop's acquire. C2: linearise a push at
the publish that covers it and a pop at its successful compare-and-swap;
cells are claimed in index order per segment and segments in list order,
which is the push order. An empty answer is linearised at the read of
`enq` (or of `next`, for a drained full segment) that found nothing beyond
`deq`: then `deq` equalled `enq` and no later segment existed.

Segments are freed through hazard pointers, as in Lemma 4, with one
shortcut: a consumer's pop slot (`hazard::kPopSlot`) keeps the head segment
from one pop to the next, and a pop that finds the slot already holding the
segment `head_` names uses it without publishing again. That is sound
because a segment is retired only after `head_` has moved past it, and a
retired segment is freed only by a scan that finds no slot holding it: a
segment the slot has held continuously since it was protected (published,
then found still the head) cannot have been freed, so it cannot have been
reused at the same address and become the head again, and finding it the
head now means it is still the head and still protected. The slot is held
continuously only if nothing else writes it between the two pops and
during the taker. Pushes use the other slot (`hazard::kPushSlot`) on every
queue, so a push from the taker (the element's move, or the destructor of
the object it is moved over) leaves the pop's slot alone; a pop from the
taker would overwrite it, and the element type's move, copy and destructor
must therefore not pop from a queue, the rule the general-purpose queue
already needs for the same reason. When another pop on the same thread
has written the slot in between, it no longer equals the head and the pop
publishes again. The element is never lent
(the shim does not choose `spsc_inline` here), so there is no Lemma 2
obligation.

**Lemma 4 (segmented queue, both forms).** `segment_queue<T, many>` is a
contract queue with no precondition beyond the destructor's ordering;
`segment_queue<T, one>` is a contract queue whenever exactly one thread
ever pops.

*Sketch.* A push takes its cell by fetch-and-add on the segment's `enq`
counter (`runtime/segment_queue.hpp`, "Design"), writes its element, then
marks the cell full with a release (store or compare-exchange); a pop
reads the mark with an acquire before the element, which is C4.

*Multi-consumer form.* A pop takes its cell by fetch-and-add on `deq`;
cells are consumed in index order per segment and segments in list order.
A pop that finds its cell unreserved waits briefly and then marks it
skipped, and the push that later takes that index moves on, so an element
never lands in a cell that has been passed; a pop that finds its cell
reserved waits for its mark, so a batch keeps its order (C3 for
batches), though not always its contiguity: a pop may skip a cell inside
the range a batch is about to take, before the batch takes it, and the
batch then keeps the cells before the skipped one, gives up the rest and
pushes them again, after cells other pushes may have taken. C3 allows
such a split and does not require one. C2 is the linearisability of the
fetch-and-add array queue this form follows, with these points: a push at
the fetch-and-add on `enq` that took the cell its element ends in, not at
its mark (a push that took an earlier cell and fills it after a later
push filled its own still comes first, and the pop that waits for it
returns it), or, for elements a push places in a segment it links itself,
at the compare-exchange that links it; a pop that returns an element at
the later of its fetch-and-add on `deq` and the fetch-and-add on `enq`
that took its cell (a pop can take a cell before any push has, wait, and
return the element of a push that takes the cell meanwhile, while a
concurrent pop answers empty); and an empty answer at its load of `enq`
that found every taken cell already claimed by a pop.

*Tests.* The sketches of this section are exercised on the interleavings
they rely on, with a thread held at a named point (the queues' `Hooks`, a
move of the element, a pop's taker): `runtime/test/lemma_rings_test.cpp`
for Lemmas 1 to 3 (a push not yet published, a consumer held inside its
pop while the producer laps the ring, a borrowed element across laps,
consumers waiting for each other's turn) and `lemma_segment_test.cpp` for
the multi-consumer form (a held push is waited for, pushes ordered by the
cell they take, a pop that returns a push which took its cell after it
did, a segment kept for a held reader, a batch split by a skip), each with
a stress run in which every empty answer is checked against the pushes
that had finished before the pop began, the single-consumer form's
included.

*Single-consumer form.* The pop takes no fetch-and-add: it takes full
cells in index order, and at a cell that is not full it walks on among the
cells pushes have taken to the first full one, keeps the cells it passed
as holes, looks at the holes again until the look finds no earlier full
one, and takes the earliest full cell out of order (the dequeue of the
Jiffy queue). C2 is shown without naming the linearisation, through the
characterisation of queue linearisability by forbidden patterns
("Aspect-oriented linearizability proofs", CONCUR 2013), in the simpler
form it takes when every value is pushed once and the pops are totally
ordered in real time, as the one consumer's are, by its program order:
such a history is linearisable if and only if no pop returns a value never
pushed or one already returned; no pop returns y while x, whose push
finished before y's push began, is returned by a later pop or not at all;
and no pop answers empty while some x, whose push finished before that pop
began, has not been returned by an earlier pop. (With no pop overlapping
another, an element that covers any part of an empty pop's interval and
was pushed before it began stays in the queue for the whole of it, so one
such element is the general condition's witness.) Elements are distinct
here (each lives in its own cell). The first condition holds because a pop
returns only a cell it saw marked full and takes each cell once: it moves
its head past a cell it takes in order and marks taken a cell it takes out
of order, and steps over taken cells. The other two follow from one fact:
if a push finished before a look at its cell began, the look sees the cell
full, since the push's fetch-and-add and mark are ordered before the pop's
acquire loads of the count and the mark. Hence an empty answer is given
only when every push that finished before the pop began has been returned,
and it linearises at the call's start. And if x's push finished before y's
push began, x took the earlier cell and x's mark is ordered before y's;
the last look over the holes, which follows the acquire load that saw y's
cell full, sees x's cell full unless x was returned already, so x is
returned first. Before 26 Oct 2026 this form answered empty at the first
cell that was not full even when a later push had finished, the third
pattern: in that window the substitution of `mpsc` for the general-purpose
queue, whose pop waits for such a cell, could add an empty answer the
program as written never gives
(`runtime/test/segment_queue_empty_test.cpp`).

Segments are freed through hazard pointers (`runtime/hazard.hpp`): a
thread protects the segment it reads with a sequentially consistent store
and a reload, and a segment is retired only after the tail and the head
have moved past it, so no thread frees a segment another may still read.
The elements themselves are moved into cells and out of them, so the
element's memory never crosses threads except as the queue's own.

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
  spmc pop retries when another consumer claimed its cell.
  `decisions/0004` answers, per operation, what each implementation can
  wait for, and the lock-free check reports it. What is claimed is that a
  substitution never introduces a wait the general-purpose queue would not
  also have had: no consumer of the spmc queue waits for another, since the
  move of the element out of its cell (and, for the pop that returns a
  `shared_ptr`, its allocation) runs after the claim, holding nothing
  another consumer waits on (`runtime/test/lemma_spmc_liveness.cpp`). The
  ring's slip is a further wait, but bounded: it ends after a fixed number
  of pauses whatever the producer does.
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
