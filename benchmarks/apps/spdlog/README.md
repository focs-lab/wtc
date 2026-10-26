# spdlog on WTC

The asynchronous logger of spdlog, v1.14.1: logging threads format a
message and hand it to a pool of worker threads through one queue, and the
workers write it to the sink. It is the first application the evaluation
takes whose grain is fine enough for the queue to matter, a message every
few hundred nanoseconds, and whose code the analysis reads as written: the
queue is a member of a pool built behind a `shared_ptr`, reached by the
logging threads through a `weak_ptr`, and its workers are started by the
pool's constructor in a loop over a count given at run time.

## Licence

This directory is under spdlog's MIT licence (`LICENSE`, the text of the
spdlog tree), because what it produces is derived from spdlog. The rest of
the repository is under the Apache License 2.0 with LLVM Exceptions.

## What the port changes

`port.py` copies spdlog's `include/` and changes the pool's queue: in
`thread_pool.h` the member is a `wtc::queue<async_msg>` with a `wtc_bound`
beside it, and in `thread_pool-inl.h` the pool calls the functions of
`wtc_q.h` (here) where it called the queue's methods; its docstring lists
every line. The message, which can be moved but not copied, is moved into
the queue's cell and out of it (`try_pop(T&)`), so nothing is allocated for
it on either side. The bound of the original queue is kept as the pushes
minus the pops, checked by a producer once per 64 of its pushes, so it
holds to within that, and a producer the bound stops spins where the
original sleeps on a condition variable; a worker spins, then yields,
where the original sleeps on one too. These are the three ways the port is
not the original; all are stated here. Waiting by spinning favours
whoever does it while there are free cores, so the leg also runs
`spdlog-orig-spin`: spdlog's own queue, mutex and ring unchanged, waiting
the way the port does (`spin_original.py`). Against it the port's gain is
the queue's alone. The sampling state of the bound is per
thread, not per pool, so the port assumes one pool per process, which is
what spdlog's registry builds. The queue is a member of the pool, not of a
queue class of its own, so that one constructor builds it and starts the
workers: that is what lets the compiler choose the tag when the pool is
built.

## What the compiler finds

| build | what the analysis says |
|---|---|
| `spdlog-generic` (`--no-specialize`) | the pool's queue, `q_ in any spdlog::details::thread_pool`: many producers (the logging threads come through the logger's virtual `sink_it_`), consumers one per iteration of the pool constructor's loop over `threads_n` |
| `spdlog-compiler` | the same, and the tag chosen when the pool is built: the single-consumer queue when `threads_n` is one, the general-purpose queue otherwise |
| `spdlog-hand` | the tag declared `mpsc` in the source; the check reads the same facts and reports that it cannot verify the declaration, since the pool is one of the objects an allocation site builds and each may have its own threads (a warning, not an error: the declaration is accepted as the program's own claim) |

The pool's queue is spdlog's one multi-producer, multi-consumer queue
whatever the number of workers, because that number is only known when the
pool is built. The compiler makes the choice there.

## Building and running

    benchmarks/apps/spdlog/fetch.sh <spdlog-dir>
    benchmarks/apps/spdlog/build.sh <spdlog-dir> <out-dir>
    <out-dir>/bin/spdlog-compiler --threads 4 --n 1000000 --payload 64 --sink null --cpus 4,5,6,7,8

`bench.cpp` is the leg's program: the given number of logging threads each
write `--n` messages of `--payload` bytes to a null sink or a file through
one logger whose pool has `--workers` threads (one by default); it reports
messages per second, from the first call to the moment the workers have
written the last, and the latency of the log call on the logging threads.
The leg `eval-spdlog` in `benchmarks/scripts/campaign.sh` runs every build
across thread counts and payloads.
