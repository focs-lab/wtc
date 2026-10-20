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

## Results so far

**RQ1** (`benchmarks/results/2026-10-21-reach`). 35 queues in the
examples, the compiler-built workloads, ferret and dedup: 21 exact, 4
conservative, 10 refused, none wrong. Of the 22 queues a reader would call
single-producer single-consumer, 15 are proved so and specialised. The
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

**RQ2, applications, first look.** Both PARSEC programs are
coarse-grained: a dedup chunk spends tens of microseconds in compression
per queue operation, and ferret's stages take milliseconds per image. In
untimed runs every build of each was within a few percent of the others;
the timed legs `eval-ferret` and `eval-dedup` are what will say it. The
grain at which the queue stops mattering is what `eval-w3grain` measures.

The timed legs for RQ2 to RQ4 are prepared and checked on an unreserved
host but not yet run.

## Corrections

The delays of the batching table in `docs/mvp.md` were sampled every 64th
element, and the batch held 64: every sample was the first element of a
batch, the one that waits longest for the rest, so those delays overstate
batching's price (paced at a million elements a second on an unreserved
host, the aligned sample's median was 63 µs where an unaligned one gives
about 32). The evaluation samples one element in 61 and re-measures them
in `eval-w7`.

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
- **Specialisation needs a single producer.** Until the shim has
  multi-producer and multi-consumer rings, the applications gain only
  where a stage has one thread; with more, the verdicts are exact but
  there is nothing to substitute.
- **The inline ring is bimodal** in some placements (`runtime/spsc_ring.hpp`);
  distributions are reported, not only medians.
- **The oracle is a reading.** Each queue's topology in
  `benchmarks/reach/oracle.json` was read from the source by hand; where
  the analysis disagrees, a build that removes the stated cause checks the
  reading.
