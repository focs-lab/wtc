# Lifting the queues programs write themselves

Until 29 Oct 2026 the compiler saw only queues written against the shim,
`wtc::queue`; a program had to be ported to it. Real programs write their
own: a class that wraps a standard container under a `std::mutex`, with a
`std::condition_variable` to wait on. `--wtc-lift-locked-queues`
(`mlir/lib/WTC/Transforms/LiftLockedQueues.cpp`) recognises such a class in
CIR, proves the substitution legal, and replaces the class's operations
with those of `wtc::lifted<C>` (`shim/lifted.hpp`), a thin layer over
`wtc::queue` and its blocking forms. From there the pipeline is the one ported code takes: the
lifting pass raises the push and pop to `wtc.queue_push` and
`wtc.queue_pop`, the topology and ownership analyses run unchanged, and
`--wtc-specialize` rewrites the constant tag that `wtc::lifted<C>::construct`
passes, as it rewrites `wtc::queue`'s. A class whose container is a
bounded ring of its own, such as spdlog's, becomes `wtc::lifted_bounded<C>`
over `wtc::bounded`, a queue with the ring's exact capacity and its
answers to a full queue ("The bound").

## The contract

The claim is that lifting is faithful: every run of the lifted program is,
in everything the contract lets a program observe, a run the original
program allows. Nothing is claimed outside the contract, and every way in
which a lifted program is known to differ is listed below, with the
remark by which the pass refuses each shape it cannot keep.

What the contract takes as given:

- **The whole program.** The module the pass sees is the program, as far
  as the class goes: it has `main`, it is not compiled to be linked with
  files the pass does not see (`wtc-cc -c`), no file linked with it names
  the class, and every member function of the class it declares has its
  body there (condition (f) below).
- **A race-free program.** The original has no data race and does not
  depend on timing: how long a thread sleeps or waits, or the order in
  which the scheduler runs threads, is not behaviour it can rely on.
- **Observable behaviour.** What the program writes, its exit status, and
  the values its functions compute: in particular the elements a pop
  returns, whether a try-pop or a closable pop found one, and the order in
  which a consumer receives them.
- **The queue's semantics.** Under its mutex the original class is a
  linearisable FIFO queue. The substitute is linearisable and FIFO per
  producer, and each push happens before the pop that returns its element
  (`decisions/0003`): the results its operations return are results the
  original's mutex allows for the same operations.

Known differences, outside the contract:

- **An operation that passes no element orders nothing.** A pop that found
  the queue empty, and a close, synchronise with nothing but the queue. A
  program that read other state after such an operation, relying on the
  original's mutex to have ordered it after an unrelated critical section,
  is outside the contract; condition (c) refuses the common form of it,
  other state guarded by the same mutex.
- **Failure ends the program.** The original's push could throw
  `std::bad_alloc` from the container, and an element's move or copy could
  throw out of a push or a pop. The substitute's allocation failure, and
  an element's move that throws, end the program (`shim/queue.hpp`).
- **Element lifetimes.** The substitute moves an element a different
  number of times than the container did, and frees its own memory on its
  own schedule. A program whose element's constructors or destructor have
  effects it observes (a count of moves, a log line per copy) can tell.
- **Wake-ups.** The original woke a waiter on every push, or every waiter
  at once; the substitute wakes one registered sleeper per push. Only
  timing tells.
- **A bounded ring's capacity above 2^30** ends the program when the queue
  is built ("The bound", below), and its overrun count is approximate
  while an overrun is in flight; the class's getter lifts only where the
  count is handed back and decides nothing.

Every refusal names the condition it fails, (a) to (f) below. Refusals
added on 30 Oct 2026, after a review found shapes the first version lifted
unfaithfully:

| remark | the shape | what lifting it did |
|---|---|---|
| (f) the unit is compiled to be linked with files the pass does not see | `wtc-cc -c` | another file's copy of the class's inline member functions, compiled as written, used the storage that holds the substitute's pointer |
| (f) a file linked with this one names C | an object file, archive or shared object linked by `wtc-cc` defines or references a member function of C, or a function that takes or returns C | as above: the linker keeps one copy of each inline member function for the program, the lifted one or the original |
| (f) the module has no main | a unit that is not the program | as above |
| (f) C::f has no body here | a member function defined in another unit | as above |
| (a) F builds or destroys a local T, whose constructor or destructor may have effects of its own | a local object other than a lock or a duration, built inside an operation | the operation's body is replaced whole, and the object's effects went with it |
| (d) F starts the stop flag S other than false / indeterminate | a stop flag that starts true, is computed, or is left uninitialised by a constructor | the substitute starts open: a closable pop that returned false at once waited instead |
| (d) F notifies cv with the mutex held | a push or a close that notifies inside its critical section | a thread that saw the operation's effect under the mutex could destroy the queue while the substitute was still waking a sleeper (after the unlock, the usual form, the original itself touches the variable after that point, so a program cannot destroy it there) |
| (e) the std::queue's underlying container is neither a std::deque nor a std::list | `std::queue<T, X>` with X a container of the program's own, even one whose name carries `std::deque` | the check read the adapter's spelling; X's `push_back` may insert anywhere |
| (e) the element is built by emplace from an argument of another type than the container's | `q.emplace(i)` with `i` an `int` into a queue of `long` | the substitute's push received the argument as the element, and the build failed |

| (d) F waits with a time limit only for an element in a class that also closes | a bounded ring's `wait_for` pop in a class with a stop flag | the substitute's timed pop gave up at `close()`, where the original's took an element pushed before its time was up |
| "may overrun" where it said "never runs" (a member or parameter now unknown) | a policy member written by value-initialisation, `memcpy`, a pointer to member, a function with no body here, `calloc`, or a function another file calls | the overrunning push was pruned, the queue built without overruns, and the program aborted when the push ran |

A move-only element (`std::unique_ptr<T>`, the common task) lifts: the
runtime's copying push is compiled for it but never called. A push of
the split layout (several producers, no overrun) no longer refuses as
full when other threads push and pop past the tail it read before it
reads the head: it refuses only once a third read finds the tail
unchanged (`bounded_push_steps split`).

## The shape

```cpp
template <typename T> class shared_queue {        // g3log, shared_queue.hpp
    std::queue<T> queue_;  std::mutex m_;  std::condition_variable data_cond_;
public:
    void push(T item) {
        { std::lock_guard<std::mutex> lock(m_); queue_.push(std::move(item)); }
        data_cond_.notify_one();
    }
    void wait_and_pop(T& popped_item) {
        std::unique_lock<std::mutex> lock(m_);
        while (queue_.empty()) data_cond_.wait(lock);
        popped_item = std::move(queue_.front());
        queue_.pop();
    }
};
```

A candidate is a member of a class that also has a `std::mutex` member, on
which some function calls `push`, `push_back`, `emplace`, `emplace_back`,
`pop`, `pop_front` or `front`. The container's operations are still calls
in CIR, named by their mangled names, and the pass reads them by their
demangled base names; the locks are the constructors of `std::lock_guard`,
`std::unique_lock` and `std::scoped_lock` on the class's mutex, and the
critical section is what the cleanup that destroys the lock covers.

Every member function that touches the container, the mutex, a condition
variable or a flag must be one of five operations:

| operation | the body, after the lock is taken | becomes |
|---|---|---|
| push | `q.push*(v)` or `q.emplace*(v)`, `v` its parameter; returns nothing or `true` | `lifted::push` (move) or `push_copy` (copy), as the original moved or copied |
| try-pop | `if (q.empty()) return false; out = q.front(); q.pop*(); return true;` | `lifted::try_pop` |
| blocking pop | wait until not empty; `out = q.front(); q.pop*();` | `lifted::pop_wait` |
| closable pop | wait until not empty or stopped; `if (q.empty()) return false;` then as try-pop | `lifted::pop_wait_or_closed` |
| close | `stop = true;` under the mutex, then `notify_all` | `lifted::close` |

A wait is the predicate loop, written either way: `while (cond) cv.wait(l)`
or `cv.wait(l, pred)`, whose predicate is a lambda the pass reads. Two
predicates are the same when their truth tables over "the container is
empty" and "the stop flag is set" are: `!q.empty() || stop`, `stop ||
!q.empty()` and the loop `while (q.empty() && !stop)` are one predicate,
and after it the test `q.empty()` and the test `stop && q.empty()` are one
test.

Constructors may build the container with its default constructor, and
must set the flag false, by a constant (a flag that starts true, is
computed or is left indeterminate refuses: the substitute starts open);
destructors destroy it. Inside an operation the only local objects built
or destroyed are locks and durations, whose constructors and destructors
do nothing else: the operation's body is replaced whole. Those calls become
`lifted::construct(&q, tag)` and `lifted::destroy(&q)`. The class keeps its
layout: the container's storage holds the pointer to the `wtc::queue`, which
nothing else in the program touches, as the proof establishes.

## The legality proof, and what each refusal says

Conservative throughout: every doubt refuses, and a remark names the reason,
prefixed by the condition it fails.

- **(a) Confinement.** The container is touched only by the constructor,
  the destructor and member functions each of which is one of the five
  operations. A function that is not a member (a friend, a free function
  over a public member), a member function of another shape, and a copy of
  the object refuse.
- **(b) The mutex is held.** Every touch is inside a critical section of
  the class's mutex, and one operation's touches are all inside one: a
  check that lets the mutex go before it acts refuses.
- **(c) The mutex guards the queue and nothing else.** Any other member read
  or written inside a critical section, in an operation or in a member
  function of its own, refuses: the substitute has no mutex, and the
  program's other state would lose its lock. The stop flag is the
  exception the operations allow, and it must be set under the mutex.
- **(d) Waits the substitute keeps.** A predicate loop on a condition
  variable, for "not empty" or "not empty or stopped"; every push notifies
  the variable the pops wait on; a close wakes every waiter; the stop flag
  is set only by a close and never cleared. Refused: a timed wait
  (`wait_for`, `wait_until`), a wait outside a loop, a push that does not
  notify, a pop that returns at the stop flag while elements are still
  queued (discard on stop), a `condition_variable_any`, a count
  (`size()`), which the lock-free queues do not keep linearisably, a
  notification made with the mutex held. A bounded ring (below) has more
  waits and counts of its own.
- **(e) FIFO at the two ends.** A `std::queue` whose member is a
  `std::deque` or a `std::list` (its type, not the adapter's spelling), a
  `std::deque` or a `std::list`; in at the back, out at the front, the
  element pushed of the container's own type; or a bounded ring the pass
  reads from its member functions ("The bound", below). Refused: `back`,
  `pop_back`, `push_front`, `insert`, `erase`, `swap`, `clear`, a
  `std::vector`, a `std::priority_queue`, a container that is neither
  standard nor such a ring, `emplace` from an argument of another type.
- **(f) A closed world.** The class lifts only where the module is the
  whole program for it (the contract above): not with `open-world` (what
  `wtc-cc -c` passes), not in a module without `main`, not when a symbol
  in `named-elsewhere` (what `wtc-cc` lists, with `nm`, from the object
  files, archives and shared objects it links) mentions the class, and not
  when a member function of the class has no body in the module. A
  lifted member function differs from the original in what it does with
  the object's storage, and another unit's copy of it, as written, would
  read the substitute's pointer as a container.

The pass also counts, per class, every container operation and wait form
it saw (the "uses" remark), so that a refusal says what a class would need.

## What the substitution keeps, and what it changes

Kept:

- **The bound.** A standard container is unbounded, and so is its
  substitute. A bounded ring, such as spdlog's `circular_q`, becomes a
  queue with the same capacity, exactly, and the same answer to a full
  queue: wait, decline or overrun the oldest ("The bound", below). Its
  port to the shim (`benchmarks/apps/spdlog`) changed the bound and the
  overflow policy, which is what the audit criticised; the lifted form
  does not.
- **Sleeping.** A pop that waited on a condition variable waits in
  `wtc::queue::pop_wait` with the `spin_then_park` policy: a short spin,
  then sleep; it is never turned into spinning without end. A push wakes a
  sleeper only when one is registered, where the original notified on
  every push.
- **Shutdown.** The stop flag becomes `close()`: a closable pop returns an
  element while there is one and false once the queue is closed and
  empty, as the original's "wait until not empty or stopped; if empty,
  return false" does.
- **The element's handling.** A push that moved its parameter moves it, one
  that copied copies; a pop moves the element out, and a pop that copied
  it out lifts only for a trivially copyable type, where the two are the
  same. A move-only element lifts.

Changed: the known differences listed under "The contract" above
(ordering of an operation that passes no element, failure, element
lifetimes, wake-ups), as decisions/0003 states for every substitution.

## The bound

A class may bound its queue itself. spdlog's `mpmc_blocking_queue` keeps
its messages in `circular_q`, a ring over a `std::vector` that is not a
standard container; the class waits while it is full (`enqueue`), declines
and counts a discard (`enqueue_if_have_room`) or overwrites the oldest
message (`enqueue_nowait`), by the logger's overflow policy.

### Reading a ring

The pass does not know `circular_q` by name. A container that is not
standard is read as a ring (`mlir/lib/WTC/Transforms/LiftedBounded.cpp`)
when it is four unsigned 64-bit counts and one `std::vector`, and its
constructor that takes the capacity sets one count to the argument plus
one (the slots), the other three to zero and sizes the vector by the
slots. Each member function the class calls is then evaluated
symbolically, over the counts' values at entry, and compared with the
operations of a ring of that many slots, head H, tail T, overrun count O
and slots M, written as the pass canonicalises them:

| operation | what the function computes |
|---|---|
| push | the element at T assigned from the argument; T becomes (T + 1) % M; if that equals H, H becomes (H + 1) % M and O grows by one |
| front | a reference to the element at H |
| pop front | H becomes (H + 1) % M |
| empty | T == H |
| full | (T + 1) % M == H |
| size | T >= H ? T - H : M - (H - T) |
| overrun count, its reset | O; O becomes 0 |

A test of M > 0 is true, since the constructor made M the capacity plus
one. Which count is the head, the tail or O is whichever assignment makes
every function the class calls one of these. The capacity is the
constructor's argument: the ring holds at most M - 1 elements. A function
that computes anything else, a constructor of another form, and a ring
built by its default constructor (spdlog's "disabled" queue) refuse.

### What the class does with it

| operation | the body, after the lock is taken | becomes |
|---|---|---|
| push that waits | wait until not full; push | `lifted_bounded::push_wait` |
| push that may decline | `if (!q.full()) { q.push(v); pushed = true; }`, and after the lock `if (pushed) notify; else <the class's own code>` | the critical section becomes `pushed = try_push(v)`; the notification goes, the class's code for a declined push (spdlog counts a discard) stays |
| push that overruns | the ring's push, with no test: it overwrites the oldest when full | `lifted_bounded::push_overrun` |
| pop with a time limit | `if (!cv.wait_for(lock, d, pred)) return false;` then as the blocking pop, `return true` | `lifted_bounded::pop_wait_for_<unit>`, the duration passed as it is |
| size, overrun count | `return q.size();`, `return q.overrun_counter();` | the runtime's |
| reset of the count | `q.reset_overrun_counter();` | the runtime's |

The pops are the unbounded ones'. Every push must notify the variable the
pops wait on, and every pop the variable the waiting pushes wait on. A
push that waits only for room in a class that also closes is refused:
the substitute's would give up at `close()` while the queue is full,
where the original waits on.

### What the bound guarantees

The substitute is `wtc::bounded<T>` (`shim/bounded.hpp`) over
`runtime/bounded_ring.hpp`. Its capacity is exact: the queue never holds
more than the capacity, and a push is refused (or waits, or overruns) only
when it holds exactly the capacity at a point inside the call. Not
"capacity plus the operations in flight", and not less.

The argument. Pushes and pops take tickets from two counters, tail and
head; the element of a push with ticket i is the one the pop with ticket i
returns. A push takes its ticket with a compare-and-swap of tail, made only
when tail - head, read in that order, is below the capacity; a pop takes
its ticket only when head < tail, read in that order. At the moment of a
successful compare-and-swap the other counter can only have moved in the
direction that keeps the test true (head only grows, so tail - head only
shrinks), so the step is legal for a sequential bounded queue in the state
the counters describe at that moment; a refusal is decided by the second
read, at which moment the first counter can only have moved the same way,
so the queue was full (or empty) then. Each operation therefore takes
effect at one atomic step on the counters, inside its call, and the
sequence of those steps is a run of the sequential bounded queue: the
answers are linearisable, with FIFO over all producers, not only per
producer. After its ticket, a push may wait for the pop of the previous
lap to finish moving its element out of the cell, and a pop for the push
of its ticket to finish moving its element in: waits on operations
already in progress, as a thread waits for the original's mutex.

A queue whose push may overrun keeps head and tail in one 64-bit word: an
overrun takes the oldest element's ticket and its own in one
compare-and-swap, so it is one step, as under the original's mutex, and
two overruns drop two elements, never three. With two words a push could
take the slot an overrun had just freed before the overrun had pushed,
and for a moment the queue would hold the capacity plus one, which a
declining push or a size would see.

The test `bounded_test exact` (`runtime/test`) holds a pop between its
ticket and its cell and checks that pushes see the room its ticket made,
which is the case a ring that decides fullness by its cells (Vyukov's
bounded queue) gets wrong: there a stalled pop makes the queue look full
to every producer, so a declining push would discard where the original
had room. `bounded_test model` checks every answer and every size against
a sequential ring.

### What a program may still see differently

- **The overrun count.** It is incremented just after the step that
  overran, so a reader can see the queue after an overrun and the count
  before it; it is exact once the pushes in flight have returned. The
  class's getter is lifted only when its value is handed back by its
  callers and nothing else (returned, through every caller, or dropped):
  approximate counts for the user, never a decision in the program. A
  class that decides on it is refused. The size is exact: the counters at
  one instant. The discard count is the class's own, incremented after a
  declined push, as before.
- **The elements' lifetimes.** The original's vector holds capacity + 1
  default-constructed elements from construction to destruction, keeps a
  popped element's moved-from remains until its slot is written again, and
  an overrun element until a later push assigns over it. The substitute
  builds no placeholders, destroys a popped element's remains in the pop
  and an overrun element in the push that overruns it. Only the element
  type's constructor and destructor can tell.
- **Large capacities.** A capacity above 2^30 ends the program when the
  queue is built; the original allocates capacity + 1 elements up front.
  (Listed with the contract's known differences above.)
- **Failure and wake-ups.** As for every lifted queue (above): a throwing
  move ends the program; one registered sleeper is woken per push, and one
  per pop on the side of the waiting pushes.

### Producers that overrun are consumers

An overrunning push takes an element out: its thread is a consumer as well
as a producer, and the dialect's `wtc.queue_push` carries `overrun` so
that the ownership analysis counts it on both sides. Where that path
cannot run, it would cost the producers their single-consumer queue for
nothing, so the pass asks first whether any call of the overrunning push
can run. A call guarded by conditions on integers whose values the pass
can resolve (`ValueSets`, `LiftedBounded.h`) through parameters, locals,
members, `std::move`/`std::forward` and reference parameters that are
only read, in a module that has `main`, never runs when every value makes
the guard false. A member's values are the values stored into it member
by member, anywhere in the module, and that is all it can hold only while
nothing reaches an object of its record another way: the member is
unknown when an object holding the record is written whole other than
from another (value-initialised to zero), copied into as bytes or reached
through a pointer of another type (`memcpy`, `memset`, a pointer to
member), handed to a function with no body here, made from raw memory
without a constructor (`calloc`; a record with a destructor begins every
life in a constructor), initialised by a global's initialiser, or named
by a file linked with this one; a parameter is unknown when such a file
names its function. In spdlog's reduced model
(`test/Integration/Lifted/spdlog_mpmc.cpp`) the policy is the logger's
member `overflow_policy_`, set by its constructor from the argument
`async_factory` passes through `std::make_shared`, the constant `block`;
`thread_pool::post_async_msg_` calls `enqueue_nowait` only when the policy
is `overrun_oldest`, so the pass leaves the overrun out, the queue is
built without it, and the loggers stay producers only. In the real
program, built from spdlog's headers, `std::make_shared`'s control block
also hands the logger's address out as `void *` (`_M_get_deleter`),
which the pass does not follow: since 31 Oct 2026 the member is unknown
there, the overrun stays, and the loggers count as consumers too. The remark says
which: "never runs, so its overrun is left out (...)" or "may overrun, so
its callers are consumers too (...)", with the first value it could not
resolve. A queue built without the overrun aborts if an overrunning push
reaches it anyway, rather than let a producer consume from a queue proved
to have one consumer.

## How the runtime gets into the module

`wtc::lifted<C>` is a template the program never names. `tools/wtc-cc`
runs the pass once with `rewrite=false instantiations=FILE`, which lists
`template struct wtc::lifted<C>;` (`wtc::lifted_bounded<C>` for a ring)
for each class that would lift, and, when
the list is not empty, compiles the file again with those lines after it:
the program's text is unchanged. The functions of `wtc::lifted<C>` carry
both the shim's annotations (`wtc_queue_push`, `wtc_queue_try_pop_into`,
`wtc_queue_construction`) and their own (`wtc_lifted_*`), by which the pass
finds them; the `wtc::queue` calls inside them are their implementation and
are not raised to the dialect again (`insideLiftedRuntime`, `Utils.h`). A
container type that cannot be spelled at the end of the file (a lambda's,
one in an anonymous namespace) is refused. `--no-lift` builds the program
as written.

## Six real queue classes

Reduced single-file models of each, with the program's thread structure,
are the tests under `test/Integration/Lifted/`; each states what was
reduced. Read as written, without source edits (29 Oct 2026):

| program, class | verdict | why |
|---|---|---|
| CTPL, `detail::Queue` (`ctpl_stl.h`) | lifts | push and try-pop; the pool's own condition variable and mutex stay, and the element, a pointer, is copied out, which is moving it. The model's topology is many to many (two threads submit; the pool's workers and the stopping thread pop), so the general-purpose queue. The real library with the leg's driver (`benchmarks/apps/ctpl`), one submitter, is spmc: the workers and the stopping thread pop |
| g3log, `shared_queue` (`shared_queue.hpp`) | lifts | push and a blocking pop; shutdown is a message, so no stop flag. In the model, with one `Active` object, the queue is proved many producers to one consumer and becomes the single-consumer queue; the model runs correctly under ThreadSanitizer and `WTC_CHECK`. The real program (`benchmarks/apps/g3log`, the logger and its performance test as one unit) builds one `Active` per sink type and one for the worker, each popped by its own thread; the queue stands for all of them, so many to many |
| BS::thread_pool, `tasks` (`BS_thread_pool.hpp`) | refused | (c) the mutex also guards the running count, the waiting flag, the thread count and the stop flag; (b) the worker pops through `pop_task()`, which takes no lock itself; (d) `get_tasks_queued()` reads `size()`, and `wait()` sleeps until the queue is empty and nothing runs, a joint invariant of the queue and the pool |
| Bitcoin, `CCheckQueue::queue` (`checkqueue.h`) | refused | (e) a `std::vector` taken from the back, with range insert and erase; (d) the batch size is computed from `size()`; (c) the mutex also guards the idle, total and unfinished counts and the result |
| cpp-httplib, `ThreadPool::jobs_` (`httplib.h`) | refused | (c) the mutex also guards the idle count, the admission bound and the dynamic threads; (d) `enqueue` decides on `jobs_.size()`; dynamic workers wait with a time-out. Its shutdown flag alone would have been supported |
| spdlog, `mpmc_blocking_queue::q_` (`mpmc_blocking_q.h`) | lifts | `circular_q` is read as a ring of capacity `max_items`; `enqueue` waits for room, `enqueue_if_have_room` may decline (its discard count stays the class's), `enqueue_nowait` overruns, `dequeue` is the blocking pop (`dequeue_for`, used by spdlog before 1.10, the pop with a time limit). The overrun cannot run in the model, whose loggers are built with `block`, so it is left out there, and the model's queue is chosen when the pool builds it: single-consumer with one worker, else general-purpose (below). The real program, `benchmarks/apps/spdlog/bench.cpp` over spdlog's own headers, lifts and runs correctly with `WTC_CHECK` and under ThreadSanitizer; since 31 Oct 2026 its overrun is kept (the policy is read through `std::make_shared`'s control block, which the pass does not follow), so its loggers are consumers too and its queue is general-purpose |

So three of the six lift.

spdlog's queue is many producers and many consumers, because the number
of workers is known only when the pool is built; the pool knows it
before it builds its queue object, whose constructor builds the queue.
Since 30 Oct 2026 the model's tag is chosen there, as the port's is ("Reading
the threads of real programs", below): the workers' pops are followed
up through the queue object to the pool, and the queue object's
constructor is called with the tag the pool's count selects.

Bitcoin's `CCheckQueue` is not a FIFO queue to lift: its workers take
batches from the back, and the result does not depend on the order in
which checks run ("first failure wins"). It is a candidate for the work on
relaxing order (a bag of tasks rather than a queue), not for this pass,
and stays refused here.
What the three refused need is not more pattern matching but semantics
the substitutes do not have: a lock split from the program's other state
(BS, httplib, Bitcoin), a count (all three), LIFO batches (Bitcoin).

## Reading the threads of real programs

A lifted queue is only as good as the topology the analysis reads for
it, and the real programs hid their threads where the reduced models did
not. Four changes (30 Oct 2026) let the lifted programs get the verdicts
their hand ports get; each keeps the analysis's rule that a doubt counts
many, and each has a test of the shapes that must stay many.

- **A call of an alias is a call of its aliasee.** A constructor defined
  out of line is called through the front end's alias of it, which has
  no body; the call handed the object to code the analysis cannot see,
  and every queue of its type escaped (g3log's worker). Any use of an
  alias other than a call still takes the aliasee's address, and a weak
  alias, which the link may replace, is not followed: a call of it is a
  call of a function with no body here
  (`test/Transforms/QueueOwnershipAnalysis/constructor-alias.cpp`).
- **Recursion poisons only what it may rebind.** The walk follows a
  recursive function's first recurrence and no deeper, so an access
  whose queue the recursion's own parameters choose may name, deeper
  down, a queue it never saw: that access stays unresolvable and its
  type unknown. One whose queue is fixed below the recursion -- a
  global, handed through functions outside the cycle -- names the same
  queue at every depth, on the same thread, and is counted (ferret's
  loader; `recursion-poisons-one-type.cpp`).
- **Threads started from descriptors.** A function stored only into a
  field that only ever reaches a spawn as its entry, in a record the
  module keeps to itself, runs only as such a thread: it is no address
  taken, and the spawn reads the field back from the descriptors the
  call hands it (ferret's `tpool_create`). A field also called directly,
  a descriptor handed to code with no body here, or one array holding
  two entries keeps every candidate (`pool-descriptors.c`).
- **The run-time choice one call away.** decisions/0005 reads a counted
  loop's bound where the queue is built, in the same function. The loop
  may now be one call down, its bound a parameter the call passes a
  variable for (the pool's create function), and the queue's
  constructor one call down (spdlog's queue object), in which case that
  function is copied with the tag as a parameter for the one call the
  loop's function makes. A loop that may stop early is counted, since it
  starts at most one thread an iteration. Another object's queue, two
  calls of the builder, a constructor that runs more than once (the
  loop's function called twice builds the queue again under the first
  run's threads), two pools feeding one side, a bound changed before
  the loop or after the queue is built leave the static verdict
  (`loop-nested-member.cpp`, `pool-descriptors.c`,
  `Specialize/run-time-count-refused.cpp`,
  `run_time_count_rebuilt.cpp`).

## Blocking forms

The blocking pop and the close are `wtc::queue`'s `pop_wait` and `close()`
(`runtime/waiter.hpp`; the waiting pop is the dialect's `queue_pop` marked
as waiting, so the progress check sees it). `wtc::lifted<C>::pop_wait`
carries the waiting pop's annotation itself, and the queue is built with
the `spin_then_park` policy: a waiter spins briefly and then sleeps, which
measured on the pools is never slower than parking at once and matches
spinning where the gaps between tasks are short, and still sleeps, as the
original did. A class that closes must not also have a pop that waits only
for an element: once the queue is closed and empty that pop would return,
where the original waits on, so it is refused.

## C queues

C programs write the same queue as a struct and a handful of functions
over it. PARSEC's ferret (`src/queue.c`) and dedup (`queue.c`, `queue.h`)
keep a ring of pointers, a pthread mutex, two condition variables and a
count of the producers that have finished:

```c
struct queue { int head, tail; void **data; int size;   /* ferret */
               int prod_threads, end_count;
               pthread_mutex_t mutex; pthread_cond_t empty, full; };
int dequeue(struct queue *q, void **to) {
    pthread_mutex_lock(&q->mutex);
    while (q->tail == q->head && q->end_count < q->prod_threads)
        pthread_cond_wait(&q->empty, &q->mutex);
    if (q->tail == q->head && q->end_count == q->prod_threads) {
        pthread_cond_broadcast(&q->empty); pthread_mutex_unlock(&q->mutex); return -1;
    }
    *to = q->data[q->tail];
    if (++q->tail == q->size) q->tail = 0;
    pthread_cond_signal(&q->full); pthread_mutex_unlock(&q->mutex); return 0;
}
```

`--wtc-lift-c-queues` (`mlir/lib/WTC/Transforms/LiftCQueues.cpp`) lifts
them into the runtime of `shim/c_queue.h`, built on the same bounded ring
as the lifted C++ classes, so that the ownership analysis and the
specialisation apply to programs never ported.

### Reading the functions

A candidate is a struct with a `pthread_mutex_t` and a `pthread_cond_t`
member, and its functions are every function that takes a pointer to it.
There are no member functions to go by, and the ring's operations may be
helpers of the program's (dedup's `ringbuffer_*`, static inline in a
header) that the threads also use on rings of their own; so the pass reads
each function by evaluating its body symbolically along every path,
calling into the helpers it calls, with the struct's members as symbols:
the value a member had when the lock was taken, and, after a wait, a fresh
value, since another thread held the mutex in between. Arithmetic on the
indices is put in one form, so that `if (++i == n) i = 0`, `if (i + 1 >=
n) i = 0` and `(i + 1) % n` are one increment, and `h == (t - 1 + n) % n`
and `(h + 1) % n == t` one test of fullness, given that the indices stay
below the slot count, as every function keeps them. Two paths that differ
only in which way an `if` went and in the values it chose are merged. A
branch that ends the program (an assertion, `abort`, `exit`) is a check
the substitute does not make, and refuses the struct (condition (d)
below), with one exception: init's check of its own allocation.

What a path decides on is read as a truth table over four facts: the ring
is empty, it is full, the end count has reached the producers, the batch
moved something. So the predicates may be written either way round, and
`while (empty && !ended)` followed by `if (empty && ended)` is the pop
whose second test can only mean "finished".

Each function is then one of:

| operation | the body | becomes |
|---|---|---|
| init | the mutex and variables initialised; `data = malloc(sizeof(void *) * S)`; the slot count `S`, a parameter plus a constant; both indices and the end count zero; the number of producers from a parameter; nothing returned, or a constant | `wtc_c_queue_init(q, tag, S - 1, producers, ends)`, at every call of it, the constant in place of the call's value |
| destroy | the mutex and variables destroyed, the ring freed; nothing returned, or a constant | `wtc_c_queue_destroy` |
| push | wait while full; store at one index, advance it by one modulo `S`; wake the pops | `wtc_c_queue_push_wait` |
| pop | wait while empty and not finished; if empty, return a constant; else take at the other index, advance it, wake the pushes, return another constant | `wtc_c_queue_pop_wait`, its answer mapped to the two constants |
| terminate | the end count incremented; the pops woken, at least when it reaches the producers; nothing returned, or a constant | `wtc_c_queue_terminate`: the counted close |
| batched push | wait while full; move elements from the caller's ring while it is not empty, the queue not full and fewer than the limit have moved; wake the pops if any moved; return how many | `wtc_c_queue_push_batch` |
| batched pop | wait while empty and not finished; return a constant if finished; move elements into the caller's ring as above; wake the pushes if any moved; return how many | `wtc_c_queue_pop_batch` |
| helper | reads members and returns a value (dedup's `queue_isTerminated`), called only by the above | left in place, unused |

The construction is moved to the call sites of init, so that every queue
has a construction of its own, whose tag the specialisation pass chooses
for that queue; init itself is called by nothing else (its address must
not be taken). The runtime's entry points take the queue as `void *`; each
struct calls them through functions of its own, `wtc_c_queue_*.<struct>`,
which take a pointer to the struct and carry the annotations the lifting
pass raises, so that a queue's type, which the ownership analysis reads
from the pointer its operations are handed, is the struct.

### The proof

As for the C++ classes, every doubt refuses, and the remark names the
condition:

- **(a) Confinement.** The module has `main` and no file linked with it
  names the struct's functions or its objects (`named-elsewhere`, as for
  the ownership analysis; with `-c`, other units may touch the struct, so
  nothing lifts). Outside its functions the program only passes pointers
  to the struct to them, compares and frees them, and stores them in a
  local or a global of the pointer's own type (or an array of them) whose
  address goes only to loads and stores: a member touched elsewhere, a
  struct copied, a pointer handed to anything else or stored anywhere
  else (a member of another struct, which other code may read) refuses.
  A global struct initialised statically (`= { ... }`), which init never
  builds, refuses. Each function is one of the operations or a helper
  only they call. What init, destroy and terminate return must be a
  constant, which the substitute returns in its place; a value computed
  from the queue or from the allocation refuses.
- **(b) The mutex is held** from the first touch of the struct to the last,
  on every path, in every operation; init and destroy take no lock. A
  wait is given the mutex held.
- **(c) The mutex guards the queue and nothing else**: an operation writes
  only the index it moves, or the end count, and outside the queue
  nothing but the element a pop hands to its caller (`*out =
  q->data[q->tail]`) on the path that takes one: a pop that also clears
  `*out` when the queue is finished, reports through another parameter,
  or an operation that counts in memory of the caller's, refuses, as do
  init and destroy writing outside the struct, and an operation that
  wakes a condition variable outside it. A batch's ring is the caller's
  own: at every call, the address of a local of the calling function that
  goes to nothing but direct calls (not to a thread), so no other thread
  can reach it while the substitute fills it without a lock; the batch
  writes nothing of it but its two indices, and those only in the loop.
  The batch's limit is compared with the count as written: converted on
  the way into a type that does not hold all of its values (`(int)max` of
  a `size_t`), it refuses.
- **(d) Waits the substitute keeps.** A push waits while the ring is full,
  a pop while it is empty and producers remain (or while it is empty, for a
  struct with no end count), each in a loop around `pthread_cond_wait`;
  the push wakes the variable the pops wait on, the pop the one the pushes
  wait on, the termination that reaches the producers the pops'. A timed
  wait, a pop that gives up before every producer has finished, a push
  that writes over a full ring refuse. So does a branch on which an
  operation ends the program, whatever it reads (dedup's assertions that
  nothing is pushed after the queue has finished and that no element is
  null, a check of a parameter): the substitute does not make the check.
  The one allowed is init's on its allocation (dedup's
  `assert(!ringbuffer_init(...))`), which the substitute's own allocation
  makes as well: it ends the program when it fails.
- **(e) FIFO.** Elements go in at one index and leave at the other, each
  advanced by one modulo the slot count that init set with the
  allocation (an index wrapped at a constant of its own refuses). The ring
  holds pointers: a ring of `long` or `double`, of a pointer's size, is
  refused.

### What the substitution keeps, and what it changes

The capacity is the ring's, exactly: `S - 1`, the slots the original's
test of fullness lets it fill (`runtime/bounded_ring.hpp`). A waiting
thread spins briefly and then sleeps. The pop answers "finished" once the
queue is empty and as many terminations as init was told have happened;
elements pushed before the last termination are still popped. What a
termination beyond the producers does depends on how the program tests
its end count. Where every test is `<` or `>=` (`end_count >=
prod_threads`), it changes nothing, in the original and in the
substitute. Where one tests equality (ferret's `end_count ==
prod_threads`), the original goes wrong after it, its pop taking from an
empty ring or waiting for ever, and the substitute ends the program
instead, a documented change.

A batch's limit is the program's value, every value of its type: a
`size_t` of -1 is no limit, an `int` below zero moves nothing. A batch
with nothing to move (an empty ring of the caller's, a limit of zero)
only waits, as the original does; a wake-up it takes is passed on, so
that a thread with something to move, which the original's broadcast
would wake, is not left asleep.

The original's operations are done with the queue when they release the
mutex, so a thread that has seen an operation's effect (the queue
finished, its element) may destroy the queue at once, before joining the
thread that made it. The substitute's operations wake sleepers after their
effect is visible; destroy waits until no operation is still in the
runtime, and frees the queue then.

A batch is taken or appended at one step, and so are its size and its
place in the queue. The ring's head and tail, the counts of pops and
pushes taken, are the two halves of one word, and a batch (`try_push_n`,
`consume_n`) reads the word, takes k as the least of what it asks for and
what the queue allows (the elements there, or the room), and moves its
counter by k with one compare-and-swap of the word, which fails, and the
batch looks again, if either counter moved since the read. So the k
tickets are adjacent, the batch's elements are the adjacent run at the
head (or at the tail) at that step, and k is what the original would
have moved under its mutex at that step. Two consumers' batches are then
disjoint runs of the queue, each the run at the head at its own step:
the batched pop is lifted whatever the number of consumers, as the
batched push is whatever the number of producers. A batch reads a cell
only after its push has filled it, as a single pop does.

Both counters must be in the one word. With two words, as the ring keeps
them for the C++ classes' single pushes and pops, a batched pop would
read tail, take k by it and move head later, and a batched push between
the two would still see the old head: each sees the queue before the
other. With two elements queued in a queue of four, a pop and a push
both asking for ten would take two and append two, where the original
gives two and four or four and two (`c_queue_test crossing` holds one
batch at that point and runs the other). A single push or pop does not
need it: what it decides is only whether the queue is full or empty, and
the other counter can only have moved the way that keeps the answer
true ("What the bound guarantees", above). A queue lifted from C keeps
every operation on the one word.

Changed, as for every substitution (decisions/0003): the mutex's total
order and its ordering of operations that pass no element; failure on
allocation ends the program (init's own check of it, the one assertion
allowed in the struct's functions, ends it too); a push wakes one sleeper
when one is registered rather than signalling every time; a termination
beyond the producers of a queue whose program tests its end count for
equality ends the program, as above.

### Several files: `--unity`

The analysis takes one translation unit, and PARSEC keeps ferret's queue
in its library and dedup's in a file of its own. `wtc-cc --unity a.c b.c
...` compiles a generated file that includes each file, unedited, in the
order given, as one unit; their file-scope names must not clash. Each file
must mean what it means compiled apart: the macros a file defines in its
own text are undefined after it, a file that defines a feature-test macro
(`_GNU_SOURCE`, `_FILE_OFFSET_BITS`, which would configure the system's
headers for every file after it) is refused, and if a file's own text,
preprocessed alone and in the unit, differs (a macro of a header of an
earlier file reaching it), the build stops. The
benchmark scripts take the original files that hold the queue, the threads
and `main` this way, and link the rest as PARSEC builds it (for ferret, its
library without `queue.o` and `tpool.o`, whose functions the unit now
defines).

### ferret and dedup from PARSEC's sources

Reduced models of both queues, with the programs' thread structure, are
the tests under `test/Integration/LiftedC/`; the real programs are built
from the pinned PARSEC tree, unedited, by `benchmarks/apps/*/build.sh`
(`ferret-lifted`, `dedup-lifted`; 30 Oct 2026):

| program, struct | verdict | why |
|---|---|---|
| ferret, `struct queue` (`src/queue.c`) | lifts | init, destroy, push, pop, terminate, with ferret's capacity (`DEPTH - 1`) and the counted close over `prod_threads`. The model, which starts its threads with `pthread_create`, gets its tags when the queues are built, from the counts in `argv`, and so does the real program since 30 Oct 2026, whose stages start through PARSEC's own thread pool and whose loader reaches the first queue through a recursive directory scan: every queue spsc at one thread a stage, its producers' or consumers' side many at more ("Reading the threads of real programs", below). Before, its five queues were unknown |
| dedup, `queue_t` (`queue.c`, `queue.h`) | refused | "(d) queue_dequeue ends the program when (q->nProducers < q->nTerminated), a check the substitute does not make": its operations assert that no more producers terminate than there are, that nothing is pushed after the queue has finished and that no element is null, checks the substitute does not make. Otherwise its functions read as init, destroy, terminate, and the batched pop and push (`queue_dequeue`, `queue_enqueue`, through dedup's own `ringbuffer_*` helpers), with the counted close over `nProducers`; the model without the assertions lifts, as spsc with one thread a stage |

So of the eight queues read from real programs, C++ and C, four lift
(CTPL, g3log, spdlog, ferret) and four are refused, each for a reason the
remark names.

The runs below are of 30 Oct 2026, when dedup's assertions were dropped
with the replaced bodies and dedup lifted; it is refused since. Both
lifted programs computed what PARSEC's own builds compute: ferret's
output on simlarge byte for byte with one thread a stage and line for
line with four; dedup's archive of the Fedora disc byte for byte at one
and four threads. ferret lifted ran five times each with one and four
threads a stage under `WTC_CHECK` and under ThreadSanitizer on simdev;
every ThreadSanitizer report is on PARSEC's own progress counter
(`cnt_enqueue`), as in PARSEC's build, none in the substituted queue.
dedup lifted ran on the Fedora disc at one and four threads under
`WTC_CHECK` and under ThreadSanitizer: no report, the archive PARSEC's. In
an untimed run on a-p13 (node 1 in the container, three runs each) the
two programs take as long as PARSEC's builds: they are coarse-grained,
far to the right of the knee in the README's "Where the gain goes", and the queue is
not where their time goes.
