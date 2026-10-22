# Evaluation

What the evaluation asks, how each question is measured and against what,
what has been answered so far, and what could make the answers wrong. The
numbers live in `benchmarks/results/`, one directory per leg with its own
`NOTE.md`; this file says which directory answers which question.

## Questions

| | question | answered by |
|---|---|---|
| RQ1 | Reach and precision: of the queues in real programs, how many does the analysis prove, how many does it refuse and why, and does it ever claim a topology the program does not have? | `benchmarks/scripts/reach.sh` against `benchmarks/reach/oracle.json`; `tools/corpus-sync-census.py` for what unported code holds |
| RQ2 | Substitution: what does the specialised queue buy in throughput, cost per element, latency under load and scaling, against strong libraries as well as against the program as written? | legs `eval-w1`, `eval-w1smt`, `eval-w2`, `eval-w3`, `eval-w3grain`, `eval-w9`, `eval-ferret`, `eval-dedup` |
| RQ3 | Allocation: what does an element cost in allocations and resident memory with each implementation? | the mechanism runs of `eval-w1` (allocation counter, `perf` counters, peak RSS) |
| RQ4 | Batching: throughput against latency as the batch grows, for a producer's pushes and for a mutex's sections, and fairness between threads | legs `eval-w7`, `eval-w8` |
| RQ5 | Cost: compile time by pass, code size, and nothing lost where nothing is proved | `benchmarks/scripts/compile_cost.sh`; the general and compiler builds of every leg |
| RQ6 | Safety: does the compiler's output run clean under ThreadSanitizer, and does the build refuse a program that breaks a declared topology? | the lit suite (`%if tsan` runs on every positive integration test, `WTC_CHECK`, the mutation tests) |

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
the oracle records. The
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

**RQ6** (`benchmarks/results/2026-10-21-tsan-apps`). The compiler's
builds of dedup and ferret, inline rings included, run under
ThreadSanitizer without a report and with PARSEC's output, at every
thread count tried; the lit suite runs every positive integration test's
program the same way, and a negative control shows the build reports a
race when a ring is forced on a queue with two producers.

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
build of each is within 1% of the others. With many threads, the
substitutes for a single side gain 2.5 to 4.0 times (the list for many
producers) and 2.0 to 2.9 times (the ring for many consumers) over the
general queue; the ring is level with the best library or ahead, the list
reaches 63 to 94% of oneTBB's and moodycamel's queues, and the general
queue, kept where both sides are many, ends 7 to 8 times behind them at
eight producers and eight consumers. Both findings led to the segmented
queue (`runtime/segment_queue.hpp`), since 22 October the general-purpose
implementation and, in its single-consumer form, the one for many
producers; the legs whose B0 it changes are being run again.

**RQ3** (the legs' mechanism passes). On the pipeline the compiler's build
allocates nothing per record where the program as written allocates four,
and spends 785 cycles a record against 7,677.

**RQ4** (`eval-w7`, `eval-w8`). Batching a producer's pushes gains 1.8
times on the general queue at the default K = 64 and nothing on the rings,
which publish with one store already; the compiler's batching equals
batching written by hand at every K. Its price is delay, with no flush on
idle: paced at 1 M elements a second an element waits 34 to 44 µs at
K = 64. Holding a mutex across 64 critical sections is 11 to 12 times
faster when nothing happens between them, and turns into a loss somewhere
between 100 and 500 ns of work between sections, at a cost in fairness
(Jain's index 0.59 at eight threads): legal is not profitable.

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

## Threats to validity

- **One socket.** The timed host has one socket; cross-socket placements
  depend on a second host and on synchronised timestamp counters across
  its sockets, which are to be checked before any cross-socket latency is
  reported.
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
  producers, or the ring whose consumers take turns); one with several of
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
