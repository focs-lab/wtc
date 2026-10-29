# Evaluation

What the evaluation asks, how each question is measured and against what,
what has been answered so far, and what could make the answers wrong. The
numbers live in `benchmarks/results/`, one directory per leg with its own
`NOTE.md`; this file says which directory answers which question.

## Questions

| | question | answered by |
|---|---|---|
| RQ1 | Reach and precision: of the queues in real programs, how many does the analysis prove, how many does it refuse and why, and does it ever claim a topology the program does not have? | `benchmarks/scripts/reach.sh` against `benchmarks/reach/oracle.json`; `tools/corpus-sync-census.py` for what unported code holds |
| RQ2 | Substitution: what does the specialised queue buy in throughput, cost per element, latency under load and scaling, against strong libraries as well as against the program as written? | legs `eval-w1`, `eval-w1smt`, `eval-ablation`, `eval-w2`, `eval-w3`, `eval-w3grain`, `eval-w9`, `eval-pool`, `eval-fusion`, `eval-spdlog`, `eval-ferret`, `eval-dedup` |
| RQ3 | Allocation: what does an element cost in allocations and resident memory with each implementation? | the mechanism runs of `eval-w1` (allocation counter, `perf` counters, peak RSS) |
| RQ4 | Batching: throughput against latency as the batch grows, for a producer's pushes and for a mutex's sections, and fairness between threads | legs `eval-w7`, `eval-w8` |
| RQ5 | Cost: compile time by pass, code size, and nothing lost where nothing is proved | `benchmarks/scripts/compile_cost.sh`; the general and compiler builds of every leg |
| RQ6 | Safety: does the compiler's output run clean under ThreadSanitizer, and does the build refuse a program that breaks a declared topology? | the lit suite (`%if tsan` runs on every positive integration test, `WTC_CHECK`, the planted violations under `runtime/test` and `test/Integration`); `benchmarks/scripts/declared_recall.sh` for the declared-topology check's recall |

## Baselines

| | what | variants (`benchmarks/queues/adapters.hpp`, build flavours) |
|---|---|---|
| B0 | the program as written | `shim_generic`, `orig_generic`; the PARSEC programs' own builds (`ferret-orig`, `dedup-orig`) |
| B1 | strong general-purpose queues | `onetbb_cq_sp`, `moodycamel_cq_sp`, `boost_ms_queue`, `std_mutex` |
| B2 | expert single-producer queues | `rigtorp_sp`/`_inline`, `moodycamel_rw_sp`/`_inline`, `boost_spsc_sp` |
| B3 | the same source, tagged by hand | the `hand` and `hand-inline` flavours; `-hand-one` builds of the applications |
| Bh | batching by hand | explicit `batch_begin`/`push_deferred`/`batch_end`; `mutex::batch_*` |

Comparisons hold the interface fixed (`_sp` against `_sp`) and report the
best possible (`_inline`) separately. The headline for each workload is
the speed-up over B0, the ratio to the best B1, and how much of B3 the
compiler reaches.

## Method

**Machines.** Timed legs run on one host in a reservation of physical
cores with their SMT siblings offline, governor `performance`, turbo off,
nothing else running anywhere on the host for the leg's duration;
`benchmarks/scripts/check_env.sh` refuses otherwise. Placements across
sockets need a second, two-socket host and are a separate leg.

**Protocol** (`benchmarks/README.md`). Every run is a fresh process; ten
repetitions, interleaved round-robin across variants so that drift falls
on all of them alike; a discarded warm-up run; control runs of a fixed
configuration before and after, compared as medians; every configuration
normalised to its own first repetition to show drift as a direction
rather than as spread.

**Statistics.** Medians with bootstrap 95% confidence intervals of the
median, never means; ratios across programs as geometric means; rows whose
coefficient of variation exceeds 3% are marked. These follow the rules of
"Scientific Benchmarking of Parallel Computing Systems" (SC'15).

**Latency** is measured open-loop from each element's scheduled send
time, not from when it was actually sent, so a stalled producer cannot hide
the queueing it causes (coordinated omission). A run that falls behind its
offered rate is marked saturated: past that point latency grows with the
length of the run and is reported as a lower bound. Timestamps are
fenced on both sides; distributions are kept as log-linear histograms
(eight buckets per power of two); in batched runs one element in 61 is
timed, a prime, so samples never line up with batch boundaries.

**Mechanism** runs, marked `pass: mechanism`, are kept apart from timed
ones because counting perturbs what it counts: user-space hardware
counters for the whole process, allocator calls through an `LD_PRELOAD`
counter, and peak resident memory, each divided by the elements pushed.

**Applications.** PARSEC ferret and dedup are ported onto the shim by a
script applied to the pinned PARSEC sources (`benchmarks/apps/`), changing
only the queues and the thread starts. Every build's output is compared
with PARSEC's own on every run; the time is the program's own region of
interest.

**Compile cost** is measured untimed (no reservation, a steady host), ten
builds per source, medians: each `wtc-cc` step and each `wtc-opt` pass
against the same clang compiling the same source without ClangIR.

**Provenance.** A leg's `meta.json` records the commit it ran at, and the
NOTEs and this page name commits too. For the results recorded so far,
those commits belong to the history the legs ran on, before the branch
that carries them was squashed into one commit per change; the tags
`eval-as-measured` and, for the reach table, `reach-r6-as-measured` keep
that history, so every commit a result names can still be checked out.

## Results so far

**RQ1** (`benchmarks/results/2026-10-21-reach`). 35 queues in the
examples, the compiler-built workloads, ferret and dedup: 21 exact, 4
conservative, 10 refused, none wrong. Of the 22 queues a reader would call
single-producer single-consumer, 15 are proved so and specialised. Four
more, ferret's with the thread count from its command line, have their
tag chosen when they are built, from the counts of the loops that start
their threads: inline rings at one thread per stage. The
refusals and imprecisions have three causes, each shown by a build that
removes it: an access reached through recursion (ferret's loader), queues
that are elements of arrays indexed at run time (dedup), and threads
started in a function that only another translation unit calls (dedup
with its queues as globals; exact once `main` is in the same file). Where
it counts many, the analysis says why, and on dedup it names the cause
the oracle records. Rerun on 26 Oct (`benchmarks/results/2026-10-26-reach`)
with an array of queues counted as one queue, every element together:
dedup's 8 refusals become 2 exact and 6 conservative verdicts, the latter
for the other-file cause alone, and two new dedup builds with `main` in the
file and the queues still in arrays are exact on all 8 queues; 43 queues,
31 exact, 10 conservative, 2 refused, none wrong. The
census of the corpus finds, besides, far more mutexes than library queues
in unported code: 518 lines naming `pthread_mutex_t` and 3571 naming a
`std::mutex` type, against six lines using a library queue outside DPDK's
rings (`docs/corpus.md`).

**RQ5** (`benchmarks/results/2026-10-21-compile-cost`, untimed). `wtc-cc`
costs 1.65 times the same clang on the compiler-built workloads and 1.9
to 2.5 times on the small examples; most of the difference is ClangIR's
own CIR emission, and the ownership analysis takes 0.2 seconds a
workload. The specialised build's code is under 0.4% larger than the
general build's on the workloads.

**RQ6** (`benchmarks/results/2026-10-28-tsan-apps`, on the current
runtime; `benchmarks/scripts/tsan_apps.sh`). The compiler's builds of
dedup, ferret and spdlog run under ThreadSanitizer, 30 runs of every
configuration, with no report from any implementation the compiler
substituted: inline rings, the single-consumer queue (dedup with up to
eight producers, ferret, spdlog with eight logging threads) and the
lock-free queue for one producer (dedup with up to four consumers); every
output is the original program's, and spdlog's file holds every message
once. ferret's only report is PARSEC's own race on a progress counter
(`cnt_enqueue`, read by the output stage while the loader increments it),
in 24 of 210 runs of the port's builds, as often on the general queue as
on the compiler's choice, and in 74 of 90 runs of PARSEC's own build
under ThreadSanitizer. The ports of ThreadPool and BS::thread_pool
(`2026-10-28-pool-libs`, `benchmarks/scripts/pool_apps_tsan.sh`) run 30
times in each of 16 configurations, on spsc, the lock-free queue for one
producer, the single-consumer queue and the general queue, with no
report and every run's checks passed. The lit suite runs every positive
integration test's program the same way, and a negative control shows
the build reports a race when a ring is forced on a queue with two producers. The
declared-topology check's recall (`2026-10-26-declared-recall`, per
audited ring, not per real bug): of 22 declarations in DPDK's examples
and supercollider made wrong on purpose, 6 stop the build and 13 change
the check's verdict, and none is verified; of 18 programs misusing
`rigtorp::SPSCQueue`, `moodycamel::ReaderWriterQueue` and
`boost::lockfree::spsc_queue` from a second thread, all 18 stop the
build, where ThreadSanitizer reports 14 to 17 depending on the run (17
without the libraries' assertions). Of the three wrong declarations DPDK
has fixed in its history, the two mempools are modelled since 28 October
and neither is caught, before the fix or after
(`2026-10-28-mempool-history`): the threads that break them are in
another process (dumpcap's capture pool) or in other files (vhost's IOTLB
pool), and vhost's pool is stored where the walk cannot place it. The
third is a driver's ring, whose threads are the application's.

**One representation, two uses** (`benchmarks/results/2026-10-28-two-uses`,
untimed). This asks whether the progress check reads the facts that chose
the implementation. Every function declared lock-free is checked in the
specialised build and in the general one. Of 58 such functions, 3 change
their verdict between the builds, all in examples written to show it;
none of the 10 in DPDK, Cinder and spdlog does. The counts on 26 October
(`2026-10-26-two-uses`) were 3 of 54 and 0 of 10. Since then, no verdict
has changed: the four new functions are tests added with the shared
blocking answer, and the changes to the queues for many producers and
for many consumers moved only reasons.

**RQ2** (`eval-w1`, `eval-w1smt`, `eval-w2`, `eval-w3`, `eval-w3grain`,
`eval-w9`, `eval-ferret`, `eval-dedup`). With one producer and one consumer
on two cores the compiler's inline queue is 17 to 60 times the program as
written (8, 64 and 256-byte elements) and level with the best expert queue
at 256 bytes; used by pointer, its ring is level with the expert queues.
Under load it keeps up to 10 M elements a second, the end of the sweep,
where the program as written falls behind at 1 M/s and the best
general-purpose library at 2 M/s; at one element every 10 µs its p99 is
542 ns, against 5.5 µs as written and 2.1 µs for the best library. On the
three-stage pipeline the compiler's build of the unmodified source is the
hand-tagged build byte for byte, 8.9 M records a second, 9.0 times the
original. The grain sweep finds the gain a nearly constant 1.1 to 1.6 µs a
record, so the speed-up is close to (t + 1.1 µs) / t for a slowest stage
of t: above 10% only while a stage takes under about 10 µs. ferret (13 ms
an image) and dedup (57 µs a chunk) lie to the right of that, and every
build of each is within 2% of the others. With many threads, the
substitutes for a single side gain 2.5 to 4.0 times (the list for many
producers) and 2.0 to 2.9 times (the ring for many consumers, since
replaced; these legs predate the swap) over the
general queue; the ring is level with the best library or ahead, the list
reaches 63 to 94% of oneTBB's and moodycamel's queues, and the general
queue, kept where both sides are many, ends 7 to 8 times behind them at
eight producers and eight consumers. Both findings led to the segmented
queue (`runtime/segment_queue.hpp`), since 22 October the general-purpose
implementation and, in its single-consumer form, the one for many
producers; the legs whose B0 it changes were run again on 26 October
(below).

**RQ2, re-measured from 26 October** against the current general-purpose
queue (`benchmarks/results/2026-10-2[678]-*`; paired runs on a second
host whose frequency is not fixed, so ratios within a repetition, not
absolute speeds, carry over; the 27 and 28 October legs on the runtime as
it is now). The paragraph above measured against the two-lock list;
these supersede its ratios.

- The queue alone (`2026-10-28-eval-w1`, on the current runtime): the
  compiler's queue is 11× the program as written (geometric mean over
  sizes and five allocators, 5.1–27×), the topology alone 6.6× (4.5–8.7)
  and the borrow proof 1.7× on top (0.70–5.0; 3.9× at 8 B, 0.86× at
  256 B), and as much over moodycamel's ConcurrentQueue, the best
  general-purpose library of those measured, which the program as written
  about matches (0.94×); against the expert rings it is within 16%, 0.84×
  rigtorp's (0.49–1.30; behind at 8 B, 0.87–1.30 at 64 and 256 B).
  On 26 October (`2026-10-26-eval-w1`) it was 10×, 5.9× and 1.7×; the
  program as written at 8 B under glibc runs at 50 or at 16–21 M/s
  depending on the run (four of five repetitions at 50 then, one now),
  which moves most of the difference, and the compiler's queue at 8 B
  runs 1.16× its earlier rate across days.
- The ablation (`2026-10-28-eval-ablation`, on the current runtime):
  from the general queue to the single-producer ring with the element
  moved out is 18× on the hand-over (17.9×, 5.3–54), the topology 8.4×
  (5.7–12.8) and the pop form 2.1× (0.82–6.7) of it (moving the element
  out needs no proof); the borrow proof's own step, lending instead of
  popping a shared_ptr, is 1.7× in W1 (0.70–5.0, a loss at 256 B);
  against moodycamel holding the element by value the topology alone
  buys 9.0× (3.1–30); against a heap-allocating queue the diagonal is
  24×; the element in the cell alone is 7.7× on the ring and 1.3× on the
  segmented queue. On 26 October (`2026-10-26-eval-ablation`) these were
  17×, 8.3×, 2.0×, 8.8× and 24×; the leg was not quiet either day, and
  the ratios are paired.
- The pipeline (`2026-10-27-eval-w3`, after the ring's gate of PR #37):
  1.90× (glibc) and 1.81× (jemalloc), equal to the hand-tagged build, and a
  p99 of 0.27–0.29 µs at every offered rate from 0.5 to 4 M records a
  second, level with the expert rings (0.30–0.32) and 0.12–0.89× the
  program as written's. The leg before the gate
  (`2026-10-26-eval-w3`) had 1.06–1.96 µs from 1 M/s.
- The grain (`2026-10-27-eval-w3grain`): the constant is about 0.04 µs a
  record, so the gain is above 10% only for stages under about 0.5 µs
  (1.90–2.06× at no work, 1.04× at 1.1 µs, 1.00× from about 5 µs); the
  same as before the gate (`2026-10-26-eval-w3grain`).
- spdlog (`2026-10-28-eval-spdlog`, on the current runtime, every
  build made again; a quiet leg, no run above the busy gate): 2.46× over
  spdlog as released (1.16–6.55), faster in all 36 configurations, of
  which the topology's share is 1.02× (0.91–1.12); the rest is the
  general-purpose queue the program gets without proof, 2.53× the
  spinning control, while spinning instead of blocking is level with the
  original (0.96×). The p99 of a call is 3.7× shorter on the geometric
  mean but longer in 3 of 36 configurations, all at 256 bytes. The
  topology's share was 1.05× (1.01–1.13) on 26 October
  (`2026-10-26-eval-spdlog-rerun`, binaries from before the runtime
  changes of that day); it fell at one and two logging threads (0.98×
  and 1.01×, from 1.08× and 1.06×; under glibc with small messages the
  single-consumer queue is now 0.91–0.94× the general one) and held at
  four and eight (1.03–1.04×). Across the two days the other builds run
  at the rates they had and the compiler's build at 0.93× with one
  logging thread: the single-consumer queue's new pop and its gated batch
  wait cost where its worker most often finds it empty, and the leg does
  not separate the two.
- Two thread-pool libraries (`2026-10-28-pool-libs`, ThreadPool and
  BS::thread_pool ported to the shim; untimed, on one socket of the
  two-socket Xeon Gold 6230, paired within a round): with one submitting
  thread the compiler's build is 2.24× and 2.18× the library as released
  (0.99–6.25), of which the topology is 1.17× and 1.12× (0.99–1.56; spsc
  chosen when ThreadPool's pool is built with one worker, spmc otherwise
  and in BS::thread_pool at every size), the port's lock-free queue 1.7×
  against the library's locked one waiting the same way, and spinning
  instead of sleeping 1.1× (the ports have no blocking pop; their idle
  workers spin and then yield). The hand declaration equals the
  compiler's choice (1.00×). With four submitting threads the general
  queue stays (1.00×), and BS::thread_pool's port loses at one worker
  (0.79–0.92×), where the library as released is at its fastest.
- Latency under load (`2026-10-27-eval-w2`, after the ring's gate): every
  queue keeps up to 10 M/s except moodycamel's ConcurrentQueue; at every
  rate the runtime's ring, bare or through the shim, has the shortest
  median measured (the compiler's queue 0.81–0.83× as written, 0.72–0.74×
  rigtorp's ring) and a p99 below as written's (0.37× at 1.4–10 M/s, 0.84×
  at 1 M/s, 0.70× at 0.1–0.5 M/s) and level with rigtorp's (0.83–0.96×,
  moving with the repetition; the leg was not quiet). Before the gate
  (`2026-10-26-eval-w2`) the ring waited from 1.4 M/s (a median 5.1× as
  written, a p99 2.8× rigtorp's) and had a p99 of 1.15 µs at 1 M/s; the
  gate's paired A/B is `2026-10-27-ring-gate-ab`.
- Many threads (`2026-10-27-eval-w9`, on the single consumer's pop of 26
  October and the lock-free queue for one producer): the general queue,
  kept where both sides are many, is 7.9× the two-lock list (2:2 to 8:8),
  1.7× moodycamel's ConcurrentQueue and 1.6× LCRQ with producers and
  consumers split (geometric means over shapes and three allocators; least
  at 8:8), and 0.82–0.89× LCRQ, LPRQ and the fetch-and-add array queue in
  the pairwise workload (LSCQ is behind it, 1.18×); every row allocates
  once per element, the shim on the consumer's side, which likely favours
  it (inferred, not measured). The substitutes for a single side gain 1.3×
  (mpsc, unchanged by the new pop) and 4.6× (spmc, 2.1× with one consumer
  to 14× with eight) over it, 2.3× and 4.5× over moodycamel. The leg
  before (`2026-10-26-eval-w9`) had the turn-taking ring at 2.2×.
- A thread pool (`2026-10-27-eval-pool`, RQ2 and RQ3): the lock-free queue
  for one producer with the task by value in its cell is 5.5× the pool as
  written with no work, 2.7× at 100 ns a task and 1.09× at 1 µs (2.5×,
  0.81–26, over 108 configurations); the topology alone is 2.25× on tasks
  in the cell and 1.70× on heap tasks, 1.00× on both at eight workers and
  1 µs, with one configuration below 0.95 (0.89×); the leg's control
  drifted 12.9%, which the ratios, taken within a repetition, do not
  follow (2.46–2.54 from one repetition to the next). The task in the cell
  removes the per-task allocation (1.10 to 0.002 a task) and gains 1.12×
  on the general queue, 1.47× on the queue for one producer. The leg
  before (`2026-10-26-eval-pool`) ran the ring whose consumers took turns
  under one spin lock, which lost at eight workers and 1 µs (0.51× on heap
  tasks, 0.85× in the cell) and at four workers and 100 ns (0.72×, 0.81×);
  its replacement's untimed A/B on the Intel host is `2026-10-26-spmc-ab`.
- Fusing a pipeline (`2026-10-26-eval-fusion`): three stages fused into
  one loop beat three threads below about 18 ns of work a stage (up to
  33×) and lose 2.5–2.7× from about 100 ns. Like coarsening (RQ4), fusion
  needs an estimate of the work between the operations it removes.

**RQ3** (the legs' mechanism passes). On the pipeline the compiler's build
allocates nothing per record. Against the two-lock list (21 October) the
program as written allocated four per record and spent 785 cycles a record
against 7,677; against the current queue it allocates two per
record, and the cycles are 830 against 2,476 (27 October,
`2026-10-27-eval-w3`, per record; 846 against 2,396 on 26 October).

**RQ4** (`eval-w7`, `eval-w8`). Batching a producer's pushes gains 1.8
times on the general queue at the default K = 64 and nothing on the rings,
which publish with one store already; the compiler's batching equals
batching written by hand at every K. Its price is delay, with no flush on
idle: paced at 1 M elements a second an element waits 34 to 44 µs at
K = 64. Holding a mutex across 64 critical sections is 11 to 12 times
faster when nothing happens between them, and turns into a loss somewhere
between 100 and 500 ns of work between sections, at a cost in fairness
(Jain's index 0.59 at eight threads): legal is not profitable.

Re-measured on 26 October (`2026-10-26-eval-w7`): on the current queues
batching a producer's pushes gains nothing, 1.00× at every K from 1 to
1024 on all three, since the segmented queue reserves a batch of cells
with one atomic and the rings publish with one store; the 1.8× above was
the two-lock list's. The delay remains, 33 µs at K = 64.

Since 28 October the coarsening has a cost model (`decisions/0006`): the
work of one iteration outside the critical section, estimated over the
CIR, against W8's crossover for the class of host and the threads that
take the mutex. Its untimed check (`2026-10-28-w8-cost`, W8 rebuilt with
its work fixed at compile time, twelve paired repetitions) found it
refusing at 200 and 500 ns, where it ran 1.15–2.16× the batched loop at two
to eight threads, and keeping the gain with two threads at 0–100 ns
(1.67–6.25× the plain loop). On that host batching loses from about 50 ns
with four and eight threads (0.77 and 0.80 at 100 ns), against 200–500 on
the timed leg's host, and the default class's thresholds, which are not
that host's, batched there: the thresholds belong to a host, and that
host has a class of its own. Pushes get no model: W7 shows no loss to avoid.

Each leg's NOTE.md, beside its summary, gives the setup, the controls and
what the numbers can and cannot carry.

## Corrections

The delays of the batching table in `docs/mvp.md` were sampled every 64th
element, and the batch held 64: every sample was the first element of a
batch, the one that waits longest for the rest, so those delays overstate
batching's price (paced at a million elements a second on an unreserved
host, the aligned sample's median was 63 µs where an unaligned one gives
about 32). The evaluation samples one element in 61; `eval-w7` measures
34 µs (inline ring) to 44 µs (general queue) at K = 64.

The first timed runs of W2, W3, W7 and W9 kept the benchmarks' own
per-element bookkeeping (the consumer's sample vectors, and in W9 one
count of elements popped that every consumer added to) on cache lines the
measured threads also used. It held the fastest queues back, by up to 2.4
times (moodycamel's queue with eight producers and one consumer), and put
the inline rings' limit under load at 3.3 to 5 M elements a second instead
of beyond 10. 5e682b6 and 63c2ba7 keep that bookkeeping in each thread
until it ends; the five legs were run again on 22 October, and their
result directories hold only the new runs.

The spdlog CIR of the 26 October two-uses measurement holds a sleep that
was injected into the port's bound by an earlier experiment. That note's
reason for the producer's enqueue, a bound that sleeps, is that injected
sleep, and the port itself spins. Emitted again with nothing injected,
the port gives the same verdicts, and the enqueue fails on its queue's
push instead (`2026-10-28-two-uses`, "spdlog's CIR").

## Threats to validity

- **One socket per leg.** The timed host has one socket; the 26 and 27
  October legs ran on a two-socket AMD host, but each within one socket
  (socket 1). Cross-socket placements are still unmeasured, and depend on
  synchronised timestamp counters across the sockets, to be checked
  before any cross-socket latency is reported.
- **Frequency not fixed on the second host.** Its legs are paired within a
  repetition and report ratios; absolute rates are not compared across
  hosts.
  Not repeated on the current runtime (the single consumer's pop and the
  lock-free queue for one producer of 26 October, the ring's gated wait
  of 27 October): the queue on SMT siblings (W1smt, 21 October), fusion
  (its pipeline is the ring, built before the gate), batching (W7, the
  rings before the gate), PARSEC's ferret and dedup, the compile cost,
  and coarsening (W8).
- **The ports change waiting.** PARSEC's queues block on condition
  variables; every ported build spins on `try_pop`, threads never
  outnumbering cores. PARSEC's own build is reported beside them as the
  control, and its queues are bounded where the shim's are not, so
  resident memory is reported too.
- **Inputs.** PARSEC's input site did not answer: ferret's simlarge comes
  from a published image of the PARSEC tree, pinned by digest and checked
  by checksum; dedup has no simlarge there and runs on its native input, a
  public disc image checked against the archive's signed checksum.
- **Many on both sides keeps the general queue.** A queue with one
  producer or one consumer has a substitute (the ring, the list for many
  producers, or the queue whose consumers claim cells); one with several of
  each keeps the general-purpose queue
  whatever the analysis proves, so a pipeline whose middle stages each
  have several threads gains only at its ends. In the legs recorded
  here the list for many producers is itself behind the best libraries;
  it has since been replaced (see RQ2 above).
- **The saturation mark compares rates.** A run is marked when the
  producer or the consumer sustained less than 95% of the offered rate; a
  backlog that does not grow over the measured part, or clears before its
  end, goes unmarked. W2's table of the rates each queue keeps up with is
  read from the latencies.
- **The inline ring is bimodal** in some placements (`runtime/spsc_ring.hpp`);
  distributions are reported, not only medians.
- **The oracle is a reading.** Each queue's topology in
  `benchmarks/reach/oracle.json` was read from the source by hand; where
  the analysis disagrees, a build that removes the stated cause checks the
  reading.
