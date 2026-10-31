# CTPL on WTC

CTPL is a header-only C++11 thread pool. `ctpl_stl.h` keeps its tasks in
`detail::Queue`, a `std::queue` under a `std::mutex`; the pool's workers,
started by `resize()` in a loop over the pool's size, wait on the pool's
own condition variable with the queue's pop as the predicate, and
`push(f)` wraps each task in a heap-allocated `std::function` and pushes
the pointer. The leg asks what the compiler's choice adds to the library
as released, its queue class lifted without a source edit
(docs/lifting.md), against the general-purpose queue.

## Version and licence

The pin is commit 437e135dbd94eb65b45533d9ce8ee28b5bd37b6d, the head of the
library's master branch; it has no release tags. `fetch.sh` clones it and
checks the commit. The library is under the Apache License 2.0 (`LICENSE`,
its text); so is this directory, whose port is an altered version of the
library, marked as such in its first line.

## The builds (`build.sh`)

| build | what it is |
|---|---|
| `ctpl-orig` | the library as released, `clang++-18 -O2` |
| `ctpl-generic` | the same source through `wtc-cc --no-specialize`: `detail::Queue` lifted onto `wtc::lifted`, the general-purpose queue |
| `ctpl-compiler` | the same through `wtc-cc`: lifted, the compiler's choice |
| `ctpl-port` | the hand port (`port.py`: `detail::Queue` over `wtc::queue`, nothing else changed) through `wtc-cc` |

## What the compiler finds

`bench.cpp` has one submitting thread, the main one. The queue is `q in
any ctpl::detail::Queue<std::function<void (int)> *>`: one producer (the
submitter) and many consumers (the workers, and the main thread, which
clears the queue in `stop()` after joining them), so spmc, and the
lifted build's tag is rewritten to spmc; the port gets the same. The
reduced model in `test/Integration/Lifted/ctpl.cpp` has a second
submitting thread, and there the queue is many to many, as it is.

The run-time choice of decisions/0005 does not apply: the main thread
pops too, so the consumers are many whatever the pool's size.

## Running

    benchmarks/apps/ctpl/fetch.sh <ctpl-dir>
    benchmarks/apps/ctpl/build.sh <ctpl-dir> <out-dir>
    <out-dir>/bin/ctpl-compiler WORKERS [SECONDS] [WORK_NS] [INFLIGHT]

It prints the time per task, tasks per second, the CPU per task and the
voluntary context switches per task.
`benchmarks/scripts/lift_attribution.sh` runs the four builds.
