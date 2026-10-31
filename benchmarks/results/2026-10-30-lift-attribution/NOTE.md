# What the compiler's choice adds to lifted programs, 30 October 2026

**Untimed.** `benchmarks/scripts/lift_attribution.sh` with five rounds, on
a-p13, in the evaluation's container (Ubuntu 24.04), given socket 1's
cores 28-39 and their SMT siblings 68-79 (Intel Xeon Gold 6230), with
another lane's work on socket 0; frequency not fixed, threads not pinned
(spdlog's benchmark pins its own within the container's set), nothing
reserved. 30 October, 08:33 to 08:43 UTC. Every program built by its
`build.sh` at eb2e7e8 (ClangIR 1ebf89d), `build-info-*.txt`. ferret and
dedup ran again from 11:40 to 11:48 UTC, the same way, built at 9bee652:
the runtime of lifted C queues now keeps a queue's two counters in one
word, so that a batch is sized at the step that claims it, and dedup's
batched pop is lifted with several consumers. 9bee652's tree differs
from this branch's documentation commit only in two lines of
documentation; spdlog's and CTPL's rows are the first round's, since
nothing they run changed. Read the ratios as indications, the ranges as
the noise.

The question: with a program's own queue class lifted from its unedited
source (docs/lifting.md), how much does the compiler's choice of queue
add over the general-purpose queue, both lifted from the same source?
Four builds of each configuration ran in turn, round by round:

| column | build |
|---|---|
| orig | the program as released (clang-18, or PARSEC's own pthreads build for ferret and dedup) |
| generic | the unedited source through `wtc-cc --no-specialize`: the queue class lifted, every queue general-purpose |
| compiler | the same through `wtc-cc`: lifted, the compiler's choice |
| port | the hand port onto `wtc::queue` through `wtc-cc` (`spdlog-compiler`, `ctpl-port`, `ferret-compiler`; for dedup `dedup-compiler-unity`, the port built as one unit with `main`, whose analysis sees what the lifted build's sees) |

`raw.csv` holds every run, `TABLE.md` the medians, `verdicts.txt` what the
analysis said of each build.

## The verdicts

All four lifted programs measured here get their ports' verdicts:
spdlog's queue mpsc when its pool has one worker, chosen when the pool
builds its queue object; CTPL's spmc with one submitter; each of
ferret's five queues chosen when built from the counts main passes to
PARSEC's thread pool (spsc at one thread a stage); dedup's, lifted with
its batched pushes and pops, the refine queues spmc and the reorder
queue mpsc, deduplicate and compress mpmc, as its port built as one
unit. g3log is not here: ClangIR's build of it fails at run time, lifted
or not (`benchmarks/apps/g3log/README.md`).

## The numbers

| program, configuration | compiler/generic, median |
|---|---|
| spdlog, 1/2/4/8 logging threads, 64-byte messages, null sink, one worker | 1.01, 1.01, 0.94, 0.98 |
| CTPL, one submitter, 1/2/4/8 workers, tasks of 0 µs | 1.13, 1.11, 1.08, 1.12 |
| CTPL, the same, tasks of 1 µs | 0.98, **1.20**, 1.17, 1.12 |
| CTPL, the same, tasks of 10 µs | 1.02, 1.16, 0.97, 1.01 |
| ferret, simlarge, 1 and 4 threads a stage | 1.03, 1.00 |
| dedup, the native input, 1 and 4 threads a stage | 1.03, 0.97 |

- **spdlog**: the right verdict buys nothing. The lifted queue is the
  bounded ring with the exact capacity (`shim/bounded.hpp`), whose
  single-consumer form only drops a compare-and-swap on the consumer's
  side, which one consumer never contends; the producers and the
  wake-ups are the cost. Both lifted builds are 1.4-2.6x the original,
  ahead of the port at one and two loggers and behind it at four and
  eight, as on 29 Oct (#62): the port's bound is approximate and its
  waiters spin, the lifted one keeps the original's exact bound and
  sleeps.
- **CTPL**: the lock-free queue for one producer against the general one,
  1.08-1.20x where the tasks are short, nothing at 10 µs, where the
  queue is no longer where the time goes. The lifted program matches the
  hand port within the noise.
- **ferret**: coarse-grained (a query takes about 13 ms), so no queue
  shows, as before.
- **dedup**: coarse-grained too (a chunk spends about 57 µs between
  queue operations), and the runs at four threads spread by ±10%; every
  archive equals PARSEC's. The lifted build runs the lifted queue with the
  compiler's tags and the port's speed.

So the compiler's choice reaches 1.2x in one configuration of one
program here, CTPL's two workers with 1 µs tasks, and 1.1x across CTPL's
short tasks, with spdlog, ferret and dedup at 1.0; not the 1.2x in three programs the evaluation asks for. What
it would take: a lifted spdlog whose single-consumer form saves more than
a compare-and-swap (the port's queue, an unbounded list, is not the
original's bounded ring), and a g3log that runs and is proved mpsc per
object.
