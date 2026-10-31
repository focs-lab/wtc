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
passes, as it rewrites `wtc::queue`'s.

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
set the flag false; destructors destroy it. Those calls become
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
  (`size()`), which the lock-free queues do not keep linearisably.
- **(e) FIFO at the two ends.** A `std::queue` over a deque or a list, a
  `std::deque` or a `std::list`; in at the back, out at the front.
  Refused: `back`, `pop_back`, `push_front`, `insert`, `erase`, `swap`,
  `clear`, a `std::vector`, a `std::priority_queue`, a container that is
  not standard.

The pass also counts, per class, every container operation and wait form
it saw (the "uses" remark), so that a refusal says what a class would need.

## What the substitution keeps, and what it changes

Kept:

- **The bound.** Every container lifted is unbounded, and so is every
  substitute. A bounded container, such as spdlog's `circular_q`, is
  refused; its port to the shim (`benchmarks/apps/spdlog`) changed the
  bound and the overflow policy, which is what the audit criticised, and
  the lifted form does not.
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
  same.

Changed, as decisions/0003 states for every substitution:

- **Ordering.** The original's mutex totally ordered every operation on the
  queue and ordered them against the mutex's other users. The substitute
  is linearisable and FIFO per producer, and each push happens before the
  pop that returns its element; an operation that passes no element orders
  nothing. A program that read other state after a pop that found nothing,
  relying on the mutex's hand-off from an unrelated critical section, is
  outside the contract; condition (c) rules out the common form of it, the
  other state guarded by the same mutex.
- **Failure.** The original's push could throw `std::bad_alloc` from the
  container; the substitute's allocation failure ends the program, as it
  does for `wtc::queue`. An element's move assignment that throws in a pop
  ends the program too (`shim/queue.hpp`).
- **Wake-ups.** The original woke on every push (`notify_one`) or woke
  every waiter (`notify_all`); the substitute wakes one registered sleeper
  per push. A program cannot tell the difference except by timing.

## How the runtime gets into the module

`wtc::lifted<C>` is a template the program never names. `tools/wtc-cc`
runs the pass once with `rewrite=false instantiations=FILE`, which lists
`template struct wtc::lifted<C>;` for each class that would lift, and, when
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
| CTPL, `detail::Queue` (`ctpl_stl.h`) | lifts | push and try-pop; the pool's own condition variable and mutex stay, and the element, a pointer, is copied out, which is moving it. The topology is many to many (the pool's workers and the stopping thread pop), so the general-purpose queue |
| g3log, `shared_queue` (`shared_queue.hpp`) | lifts | push and a blocking pop; shutdown is a message, so no stop flag. The queue is proved many producers to one consumer and becomes the single-consumer queue; the model runs correctly under ThreadSanitizer and `WTC_CHECK` |
| BS::thread_pool, `tasks` (`BS_thread_pool.hpp`) | refused | (c) the mutex also guards the running count, the waiting flag, the thread count and the stop flag; (b) the worker pops through `pop_task()`, which takes no lock itself; (d) `get_tasks_queued()` reads `size()`, and `wait()` sleeps until the queue is empty and nothing runs, a joint invariant of the queue and the pool |
| Bitcoin, `CCheckQueue::queue` (`checkqueue.h`) | refused | (e) a `std::vector` taken from the back, with range insert and erase; (d) the batch size is computed from `size()`; (c) the mutex also guards the idle, total and unfinished counts and the result |
| cpp-httplib, `ThreadPool::jobs_` (`httplib.h`) | refused | (c) the mutex also guards the idle count, the admission bound and the dynamic threads; (d) `enqueue` decides on `jobs_.size()`; dynamic workers wait with a time-out. Its shutdown flag alone would have been supported |
| spdlog, `mpmc_blocking_queue::q_` (`mpmc_blocking_q.h`) | refused | (e) `circular_q` is spdlog's own bounded ring; `enqueue` waits while it is full, `enqueue_nowait` overwrites the oldest message, and the overrun and discard counters are read: a bound and an overrun the unbounded substitutes cannot keep |

So two of the six lift.

Bitcoin's `CCheckQueue` is not a FIFO queue to lift: its workers take
batches from the back, and the result does not depend on the order in
which checks run ("first failure wins"). It is a candidate for the work on
relaxing order (a bag of tasks rather than a queue), not for this pass,
and stays refused here.
What the four refused need is not more pattern matching but semantics
the substitutes do not have: a lock split from the program's other state
(BS, httplib, Bitcoin), a count (all three), a bound and an overrun
(spdlog), LIFO batches (Bitcoin).

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
