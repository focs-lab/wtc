# Two thread-pool libraries ported to the shim, 28 October 2026

Two thread-pool libraries as they are used, not written for this project:
ThreadPool (the single-header pool of 2014, commit 9a42ec1, zlib licence)
and BS::thread_pool (v5.1.0, MIT licence). Each is ported to the shim
(`benchmarks/apps/threadpool`, `benchmarks/apps/bs-thread-pool`): the
pool's task queue becomes a `wtc::queue`, and nothing else in the library
changes shape. The questions are the ones the spdlog port asked: does the
analysis find the topology a reader sees in a library as written, and what
does that topology buy on top of the port itself?

`benchmarks/scripts/pool_apps_leg.sh` (this directory's `runs.jsonl`,
`log.txt` and `meta.txt` are its output), `REPS=12 RUN_SECONDS=0.5`, in
the evaluation's container (Ubuntu 24.04) on the two-socket Intel host
(Xeon Gold 6230, 2 × 20 cores), the container given socket 1's twenty
cores (20-39) and not their SMT (simultaneous multithreading) siblings
(60-79), each of a program's threads pinned to a core of its own.
28 October, 09:05 to 09:50 UTC. `pool_apps_pairs.py` wrote `SUMMARY.md`
and `pairs.csv`.

Each library has two programs: one in which the main thread is the one
submitter, and one in which four threads submit. Every configuration is
1, 2, 4 or 8 workers times 0, 100, 1000 or 10000 ns of arithmetic a task
(calibrated once at the start), 16 a program; twelve rounds, each run a
fresh process of 0.5 s, the builds of one configuration back to back in
an order rotated from round to round. Every run checks its own result:
every future gives its task's value, every task runs exactly once, and
the tasks still queued when the pool is destroyed run before its workers
leave. All 4224 runs passed.

## Builds

From each library's `build.sh`:

| build | what it is |
|---|---|
| orig | the library as released, through `wtc-cc` (the same compiler as the port's builds, with no queue of the shim to change) |
| orig-clang | the same, by clang++-18: what a program gets today |
| orig-spin | the control: the library's own queue and mutex, its workers spinning and then yielding where the original's sleep on a condition variable |
| generic | the port, `wtc-cc --no-specialize`: the shim's general-purpose queue (B0, the program as written) |
| compiler | the port, `wtc-cc`: the implementation the compiler chooses |
| hand | the port with the queue declared spmc (single producer, many consumers) by hand (B3), which the compiler checks; the one-submitter program only |

What the analysis says about each pool's queue:

| library | one submitter | four submitters |
|---|---|---|
| ThreadPool | one producer, the workers started by the constructor's counted loop: the tag is chosen when the pool is built, spsc (single producer, single consumer) with one worker, else spmc | mpsc (many producers, single consumer) with one worker, else the general queue, chosen the same way |
| BS::thread_pool | spmc, for every number of workers: the workers' loop counts to a member in `create_threads()`, outside the shape `decisions/0005` covers, so there is no choice at run time | many producers and many consumers: the general queue |

The hand declaration is verified in both libraries.

## Ratios

A ratio is two builds' tasks a second within one round; its median over
the twelve rounds stands for the configuration; the figure is the
geometric mean over the 16 configurations, with the smallest and largest
median. With one submitter:

| ratio | ThreadPool | BS::thread_pool |
|---|---|---|
| compiler / orig (total) | **2.24** (1.00-5.92) | **2.18** (0.99-6.25) |
| generic / orig (the port on the general queue) | 1.90 (1.00-3.76) | 1.93 (1.00-4.46) |
| compiler / generic (the topology) | **1.17** (1.00-1.56) | **1.12** (0.99-1.35) |
| hand / compiler | 1.00 (0.98-1.06) | 1.00 (0.98-1.05) |
| orig-spin / orig (the wait alone) | 1.10 (0.95-1.42) | 1.09 (0.93-1.41) |
| generic / orig-spin (the queue alone) | 1.73 (1.00-3.20) | 1.75 (1.00-3.64) |
| orig / orig-clang (ClangIR) | 1.00 (0.95-1.03) | 1.00 (0.95-1.05) |

With four submitters:

| ratio | ThreadPool | BS::thread_pool |
|---|---|---|
| compiler / orig (total) | 2.02 (1.00-8.12) | 1.81 (**0.79**-7.52) |
| compiler / generic (the topology) | 1.00 (1.00-1.01) | 1.00 (0.97-1.01) |
| orig-spin / orig (the wait alone) | 1.02 (0.90-1.37) | 1.02 (0.89-1.46) |
| generic / orig-spin (the queue alone) | 1.97 (1.00-5.95) | 1.79 (0.86-5.36) |
| orig / orig-clang (ClangIR) | 1.01 (0.98-1.05) | 1.00 (0.97-1.02) |

The total is the product of the wait, the queue and the topology (with one
submitter, ThreadPool: 1.10 × 1.73 × 1.17 = 2.23). By work a task, with
one submitter (ThreadPool / BS::thread_pool):

| work a task | total | topology |
|---|---|---|
| 0 ns | 3.45 / 3.23 | 1.30 / 1.21 |
| 100 ns | 3.11 / 3.19 | 1.26 / 1.19 |
| 1000 ns | 2.29 / 2.17 | 1.15 / 1.09 |
| 10000 ns | 1.02 / 1.01 | 1.00 / 1.00 |

What the leg shows:

- The analysis reads both libraries as a reader does, one producer and
  many consumers with one submitter, and its choice equals the hand
  declaration (1.00×) in both.
- The topology grows with the workers: in ThreadPool from 1.17× at one
  worker (spsc, chosen when the pool is built) to 1.56× at eight, with no
  work a task; in BS::thread_pool from 1.06× (spmc even at one worker,
  since nothing is chosen at run time there) to 1.35×. At 10 µs a task
  every build is within 4% of every other: the pool is not the cost.
- Most of the total is the port itself, as in spdlog: the lock-free general
  queue against the library's locked `std::queue`, 1.7-2.0× when both
  wait the same way.
- With four submitters there is nothing for the topology to change (the
  general queue in both libraries, and ThreadPool's mpsc at one worker is
  the same segmented queue with its consumer's side unshared): 1.00×,
  which also serves as a check that the compiler's build costs nothing
  where it does not substitute.
- Compiling through ClangIR (Clang's intermediate representation in MLIR,
  the Multi-Level Intermediate Representation, which `wtc-cc` goes
  through) instead of by clang++-18 changes nothing (1.00×).

## Worst cases

- **BS::thread_pool, four submitters, one worker: the port loses**, 0.81×
  at 0 ns, 0.79× at 100 ns, 0.92× at 1000 ns (1.02× at 10 µs). The
  compiler's build and the generic one are the same there (0.97-1.01×), so
  it is the port, not the substitution. Both halves of it lose: the wait
  (orig-spin / orig 0.92-0.97×) and the queue (generic / orig-spin
  0.86-0.93×). BS::thread_pool as released is fast in this configuration,
  0.94 M tasks a second against ThreadPool's 0.68, where the two ports
  are close (0.77 and 0.72); unlike ThreadPool's, the BS port keeps a
  shared count of unfinished tasks (for `wait()` and the task counts),
  which every submission and every finished task update, five threads on
  one counter. That the counter is the cause is likely, not measured; the
  runs were not profiled. ThreadPool in the same configuration: 1.03-1.08×.
- **The topology's smallest medians** are 0.99× (BS::thread_pool, one
  submitter, one worker, 100 and 10000 ns) and 0.97× (BS::thread_pool,
  four submitters, one worker, 0 ns, where no queue is substituted): noise,
  the latter in the leg's noisiest configuration (rounds from 0.66 to
  1.32).
- **With one submitter the total never loses**: its smallest median is
  0.99× (BS::thread_pool, one worker, 10 µs).

## Under ThreadSanitizer

`benchmarks/scripts/pool_apps_tsan.sh` (this directory's `tsan.tsv` is its
output) on the compiler's builds that `TSAN=1` adds to each `build.sh`
(`wtc-cc --tsan -O1 -g`, one translation unit: the program, the pool and
the runtime's queues all instrumented), run in the same container on
node 1 on 29 October from the tree rebased onto main, whose builds are
the leg's byte for byte. 30 runs of every configuration, each to its end
(`halt_on_error=0`), symbolized: both programs of both libraries at 1, 2,
4 and 8 workers, four submitting threads in the second, 50 rounds of work
a task, 0.3 s a run, at most 256 tasks in flight a submitter. That covers
every implementation the compiler chooses for the pools: spsc (ThreadPool,
one worker), the lock-free queue for one producer (ThreadPool with more
workers, BS::thread_pool at every size), the single-consumer queue
(ThreadPool with four submitters and one worker) and the general queue
(both with four submitters).

All 480 runs end with no report and pass their own checks: every future's
value, every task run exactly once, the tasks queued at destruction run
(and in BS::thread_pool, `wait()` returning only when every task has run).

## The wait

The ports' workers do not sleep: a worker with nothing to do pauses 1024
times, then yields, until it has a task. The original's sleep on a
condition variable. Each of the leg's threads has a core of its own, so a
spinning worker costs nothing the leg can see; on an oversubscribed host,
or where energy counts, it would. The control `orig-spin` isolates that:
the library's own queue and mutex, waiting the way the port does. Spinning
gives the libraries as released 1.09-1.10× with one submitter (up to
1.42× at eight workers and no work, and 0.93× at worst) and 1.02× with
four; the rest of the total is the queue and the topology. The
comparison is fair to the libraries only in that sense: their own
waiting, once it spins too, gains about a tenth, not the 2×.

The shim's queues have no blocking pop yet; PR #59 adds one (`pop_wait`,
`pop_wait_for` and `close`, with spinning, parking and spin-then-park
policies), after which the ports can sleep the way the libraries do.

## How noisy

The leg reserved nothing and fixed no frequency (governor `powersave`,
turbo on): it reports ratios taken within a round, not speeds to compare
across hosts. What else ran:

- It started at 09:05 UTC once no other container held a processor of
  node 1 (`wait.log`: 35 minutes of waiting, for this project's own
  ThreadSanitizer run and then for short-lived containers). Socket 0 ran another
  lane's work throughout; the host's load average (80 hardware threads,
  every tenant) was 20 to 50, 34 at the median (`log.txt`, once per
  configuration).
- `node1-busy.txt` samples, every 30 s, the busy share of node 1's
  processors over every tenant. The leg's cores 20-39: 0.48 at the median
  (0.30-0.64), most of it the leg itself, whose ports spin. Their SMT
  siblings 60-79, which the leg did not use and which share a physical
  core with its threads: 0.12 at the median, above 0.25 in 9 of 89 samples,
  in three bursts (09:15-09:17, 09:20-09:21, 09:44-09:45), 0.49 at most.
- The original build's rate, against its own median, stays within 3% from
  round to round except in rounds 2 and 10 (0.84 and 0.92), which fall in
  bursts above. The ratios do not drift: the median of the last six rounds
  over the first six is 0.99-1.00 in geometric mean for every ratio (0.88
  to 1.10 for single configurations).
- Within a configuration, the rounds' ratios spread by 5-8% at the median
  (interquartile range over median), and by 23-27% at most, in BS::thread_pool
  with one worker and four submitters and at eight workers.

Read a ratio within 5% of 1 as equal. Every conclusion above rests on
ratios well outside that, or on configurations where the builds are
equal by construction.

## Provenance

`meta.txt`'s `wtc=` and ClangIR revision are empty: the leg ran from a copy
of the tree on that host, not a git checkout. The binaries were built from
this branch's ports and leg script over main at abe26c6 (ClangIR 1ebf89d).
After the branch was rebased onto main at f0daebb, every build was made
again from the rebased tree (`TSAN=1`, both `build.sh`): all 26 binaries,
the ThreadSanitizer builds among them, are byte for byte the ones the leg
ran, and the analysis's verdicts (`verdicts.txt`) are the same. The `cmd`
field in `runs.jsonl` gives each run's command line, relative to the
copy's root. `wait.log` is the leg's wait for node 1, `node1-busy.txt` its
busy record.
