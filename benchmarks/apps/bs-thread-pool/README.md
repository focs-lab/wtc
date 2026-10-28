# BS::thread_pool on WTC

BS::thread_pool is a single-header C++17/20/23 thread pool (about 2,500
lines). Its default pool keeps one `std::queue` of `move_only_function`
tasks under the pool's mutex. Beside the queue it keeps a count of
running tasks and two condition variables: one wakes the workers, the
other wakes `wait()`. Tasks arrive through `detach_task`, through
`submit_task`, which hands back a future, and through the bulk
submissions, which all go through `detach_task` or `detach_bulk`. The
workers are started by `create_threads()`. The constructor calls it, and
so does `reset()`. The port asks the same two questions as the ThreadPool
port (`../threadpool`): does the analysis find the topology a reader
sees, and what does that topology buy? It asks them of a library whose
structure is less simple.

## Version and licence

The pin is v5.1.0, commit bd4533f1f70c2b975cbd5769a60d8eaaea1d2233.
`fetch.sh` clones the tag and checks it against that commit. The library
is under the MIT licence: `LICENSE` holds the text of its `LICENSE.txt`.
This directory is under the same licence, because what it produces is
derived from the library. Its source is not in this repository: `port.py`
makes the altered header from a clone. The rest of the repository is
under the Apache License 2.0 with LLVM Exceptions. The library is built
as C++17, where its tasks are its own `move_only_function` (one heap
allocation per task) and its threads are `std::thread`.

## What the port changes

`port.py` writes an altered `BS_thread_pool.hpp` and marks every changed
line "WTC port"; its docstring lists them.

- The member `tasks` is a `wtc::queue<task_t>`.
- `wtc_counts` takes the place of `tasks_running`, `waiting` and
  `workers_running`. It holds three things: the tasks submitted and not
  finished (as the original counts them, including each worker until its
  initialisation is done), the threads in `wait()`, and the stop. A flag
  per worker says whether it runs a task.
- `detach_task` and `detach_bulk` count the task and push it. They take
  no mutex and send no notification.
- The worker marks its last task (or its initialisation) finished. It
  then pops, spinning and then yielding, until it has a task or the pool
  is stopping.
- `wait()`, `wait_for()` and `wait_until()` sleep on the original's
  condition variable, under the original's mutex, until nothing is
  unfinished. The last task to finish wakes them; `wtc_q.h` explains why
  no wake-up is lost.
- `get_tasks_queued()`, `get_tasks_running()` and `get_tasks_total()`
  read the counters, and `purge()` pops the queued tasks and drops them.
- `pop_task()` is removed.

Everything else stays the library's: the constructors, `create_threads()`
with its loop, `submit_task()` and the `task_and_future` it builds, the
futures, the init and cleanup functions, the thread-local pool and index,
and `reset()`.

Three things differ from the original:

- **Idle workers spin.** A worker with nothing to do spins, then yields,
  so it costs a core.
- **The task counts are snapshots.** They are read from counters, not
  from one locked state, so `get_tasks_queued()` and
  `get_tasks_running()` can disagree with `get_tasks_total()` for an
  instant.
- **Priorities and pausing are not ported.** A `static_assert` in the
  class says so.

The leg's control, `orig-spin` (`port.py --orig-spin`), keeps the
library's queue, mutex and counts. Its workers spin, then yield, on a
count published under the mutex, where the original's sleep on the
condition variable. This is the same control as for ThreadPool.

## What the compiler finds

| build | what the analysis says about `tasks in any BS::thread_pool<BS::tp::none>` |
|---|---|
| `generic` (`--no-specialize`) | spmc: one producer, the main thread; many consumers, "reached by a call in a loop at BS_thread_pool.hpp" (the loop in `create_threads()`) |
| `compiler` | "specialised: constructor tag rewritten to spmc", for every number of workers |
| `hand` | "declared spmc, verified" |
| `multi-generic`, `multi-compiler` | mpmc: many producers (the submitting threads) and many consumers; the general-purpose queue stays |

The analysis proves the topology a reader sees, and the compiler
substitutes the queue for one producer statically. It does not make the
choice at run time, so with one worker the queue is still the one for
many consumers. Three things in the library's structure stand in the
way, and decisions/0005 names each as outside the shape it covers:

1. **The bound is a member.** The workers' loop counts to `thread_count`.
   A counted loop's bound must be a variable or a global, so the loop is
   not counted at all, and no remark says which rule failed: the queue's
   remark only says that its consumers are reached by a call in a loop.
2. **The loop is not in the queue's constructor.** `thread_count` is
   written by `determine_thread_count()` inside `create_threads()`, and
   that is where the loop is. The queue is built in the constructor.
3. **The loop's function runs more than once.** `reset()` calls
   `create_threads()` again. That call is not compiled here, since the
   program never calls `reset()`. Proving the rest would still need a
   pool whose workers are started once.

The library's code is not changed to get past any of these. The result
is reported as it stands.

## Building and running

    benchmarks/apps/bs-thread-pool/fetch.sh <dir>
    benchmarks/apps/bs-thread-pool/build.sh <dir> <out-dir>
    <out-dir>/bin/bs-thread-pool-compiler --workers 4 --rounds 750 --work 1000

`build.sh` lists the builds. `bench.cpp` is the leg's program. It is the
ThreadPool program with `submit_task` in place of `enqueue`, and it adds
checks of `detach_task` and `wait()`:

- after `wait()` returns, every task submitted has run, and the pool
  counts no task unfinished, queued or running;
- the destructor waits for the tasks still queued.

`benchmarks/scripts/pool_apps_leg.sh` runs the leg, and the results are
in `benchmarks/results/2026-10-28-pool-libs/`.
