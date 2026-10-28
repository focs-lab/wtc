# ThreadPool on WTC

ThreadPool is a single-header C++11 thread pool of about a hundred lines,
and the pattern many hand-written pools copy. The constructor starts N
workers. One `std::queue<std::function<void()>>` sits under a mutex and a
condition variable. `enqueue(f, args...)` wraps the call in a
`packaged_task` and hands back its future. The destructor lets the
workers drain the queue and joins them. The port asks two questions of
library code as written. Does the analysis find the topology a reader
sees: one submitter, N workers? And what does that topology buy, against
the general-purpose queue and against the library as released?

## Version and licence

The pin is commit 9a42ec1329f259a5f4881a291db1dcb8f2ad9040 of 26
September 2014, the head of the library's master branch; the library has
no release tags. `fetch.sh` clones it and checks the commit. The library
is under the zlib licence: `LICENSE` holds the text of its `COPYING`.
This directory is under the same licence, because what it produces is an
altered version of the library. As the licence asks, the altered header
says so in its first lines. Its source is not in this repository:
`port.py` makes it from a clone. The rest of the repository is under the
Apache License 2.0 with LLVM Exceptions.

## What the port changes

`port.py` writes an altered `ThreadPool.h` and marks every changed line
"WTC port"; its docstring lists them all.

- The member `tasks` is a `wtc::queue<std::function<void()>>`. Like the
  queue it replaces, it is built by the pool's constructor before the
  loop that starts the workers.
- `stop` is a `std::atomic<bool>`, and the mutex and the condition
  variable are gone.
- A worker takes a task with `wtc_pool_pop` (`wtc_q.h`). It spins, then
  yields, until it has a task. It returns false once the pool is stopping
  and the queue is empty, which is the condition the original slept on.
- `enqueue` checks `stop` and pushes. It takes no mutex and sends no
  notification.

The task, the `packaged_task` and the future are the library's own, and
so is the constructor's loop.

Two things differ from the original:

- **Idle workers spin.** A worker with nothing to do spins, then yields,
  so it costs a core.
- **`enqueue` does not serialise against the destructor.** In either
  version, an `enqueue` concurrent with the pool's destruction uses the
  pool after its lifetime has ended. Only the original turns that into an
  exception.

Waiting by spinning favours the variant that does it while cores are
free, so the leg also builds a control, `orig-spin` (`port.py
--orig-spin`). It keeps the library's queue, mutex and condition variable.
Its workers spin, then yield, on a count published under the mutex, where
the original's sleep on the condition variable; then they take the mutex.
The original against the control is the cost of sleeping. The control
against the port's general build is the cost of the locked queue against
the lock-free one, when both wait by spinning.

## What the compiler finds

| build | what the analysis says about `tasks in any ThreadPool` |
|---|---|
| `generic` (`--no-specialize`) | spmc: one producer, the main thread; consumers one per iteration of the constructor's loop over `threads` (ThreadPool.h:41), "so one when it runs at most once" |
| `compiler` | the same, and the tag chosen when the pool is built: "spsc when the loop at ThreadPool.h:41:5 (consumers) runs at most once, else spmc" |
| `hand` | "declared spmc, verified": the topology is written in the source, and the check confirms it |
| `multi-generic` | mpmc: many producers (the submitting threads, started in a loop in `main`) and the consumers as above |
| `multi-compiler` | "mpsc when the loop at ThreadPool.h:41:5 (consumers) runs at most once, else generic" |

The library's own shape is the shape decisions/0005 covers. The queue is
a member that the constructor builds before its loop. The loop counts to
the constructor's parameter, and nothing writes that parameter after the
queue is built. So the run-time choice applies to the library as written,
with no change to its structure. In the program with several submitters,
the producers' loop is in `main`, not in the function that builds the
queue. That side therefore stays "many", and the choice is made on the
workers' side alone.

Every pop moves the task out of its cell (`try_pop(T&)`). Nothing is
lent, so no borrowing proof is needed. The element type,
`std::function`, is not trivially destructible and could not be lent in
any case.

## Building and running

    benchmarks/apps/threadpool/fetch.sh <dir>
    benchmarks/apps/threadpool/build.sh <dir> <out-dir>
    <out-dir>/bin/threadpool-compiler --workers 4 --rounds 750 --work 1000

`build.sh` lists the builds, among them `orig-clang`: the library compiled
by clang++-18 instead of through `wtc-cc`. `bench.cpp` is the leg's
program. Its comment says what a run measures, and the checks each run
must pass:

- every future gives its task's value;
- every task submitted runs exactly once;
- the tasks still queued when the pool is destroyed run before the
  workers leave.

`benchmarks/scripts/pool_apps_leg.sh` runs the leg, and
`benchmarks/scripts/pool_apps_pairs.py` takes its ratios. The results are
in `benchmarks/results/2026-10-28-pool-libs/`.

One limitation of ClangIR surfaced on the way. A function passed to
`enqueue` by reference (`enqueue(task, id)`, with `task` a function)
reaches LLVM as a `dereferenceable(0)` argument, which LLVM rejects. The
driver passes a lambda instead.
