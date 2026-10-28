# The applications under ThreadSanitizer, 28 October 2026

The safety run of `2026-10-21-tsan-apps` repeated on main at abe26c6,
whose runtime changed under the compiler's builds since then: the
single-consumer pop takes the first filled cell past one a push has taken
and not yet filled (acac0fe), the queue for one producer and many
consumers is a lock-free queue instead of the ring whose consumers took
turns (0208242, 778cc97), and the ring's and the segmented queue's
consumers wait for a batch only while the producer streams (ffaa22d,
1d914d9). A new implementation can race where the old one did not, so
every program that reaches one of them runs again, and the two programs
that reach the new ones but were not run before are added.

`benchmarks/scripts/tsan_apps.sh` (this directory's `runs.tsv` is its
output) on the builds `TSAN=1 benchmarks/apps/{dedup,ferret,spdlog}/build.sh`
make, in the evaluation's container (Ubuntu 24.04, clang 18's
ThreadSanitizer runtime) on the container host's second socket, 30 runs of
every configuration, each to its end (`halt_on_error=0`), symbolized. dedup
is instrumented in every file, ferret and spdlog in the translation unit
with the queues and the threads (ferret's libraries start no threads).
Each output is compared with the original program's: dedup with PARSEC's
own build at one thread per stage, byte for byte; ferret with PARSEC's own
build at one thread per stage, byte for byte or, with more threads per
stage, line for line; spdlog by counting every message in the log file.
The inputs are those of 21 October: dedup on simdev (`hamlet.dat`) and
on the first 32 MB of the native disc, ferret on simdev.

## What each build runs

From the analysis's verdicts at build time (`verdicts.txt`):

| program | build | implementations | threads per stage |
|---|---|---|---|
| dedup | compiler-tsan (as written) | every queue general: each array of queues is one queue, its threads start in a function another file calls | 1, 2, 4 |
| dedup | compiler-flat-one-unity-tsan | three inline rings; reorder: the **single-consumer queue** (2 producers) | 1 |
| dedup | compiler-unity-tsan (added) | refine: the **lock-free queue for one producer** (1, 2, 4 consumers); reorder: the **single-consumer queue** (2, 4, 8 producers); two general | 1, 2, 4 |
| ferret | compiler-one-tsan | four inline rings, the loader's queue general (reached through recursion) | 1 |
| ferret | compiler-tsan | chosen when the queues are built: at one thread per stage inline rings; at two and four the **single-consumer queue** into the output stage and the general queue | 1, 2, 4 |
| ferret | generic-tsan, orig-tsan (added) | the port on the general queue, and PARSEC's own build: the controls | 1, 2, 4 |
| spdlog | compiler-tsan | chosen when the pool is built: one worker, the **single-consumer queue** (1, 2, 4, 8 logging threads); two workers, the general queue (8) | |

The 21 October run had no build on the lock-free queue for one producer
(the ring for one producer and many consumers did not exist in a ported
program then) and did not run spdlog, whose port came later; dedup as
written then got no verdict at all. The queue for one producer is reached
only by dedup with its `main` in the encoder's translation unit and its
threads per stage from `argv` (ferret's middle stages all have the same
count, so no ferret queue has one producer and many consumers).

## Outcome

| program | build | threads | runs | runs with a report | output |
|---|---|---|---|---|---|
| dedup | compiler-tsan | 1, 2, 4 | 60 each | 0 | PARSEC's, every run |
| dedup | compiler-flat-one-unity-tsan | 1 | 60 | 0 | PARSEC's |
| dedup | compiler-unity-tsan | 1, 2, 4 | 60 each | 0 | PARSEC's |
| ferret | compiler-one-tsan | 1 | 30 | 3 | PARSEC's, byte for byte |
| ferret | compiler-tsan | 1 / 2 / 4 | 30 each | 2 / 4 / 5 | PARSEC's |
| ferret | generic-tsan | 1 / 2 / 4 | 30 each | 4 / 1 / 5 | PARSEC's |
| ferret | orig-tsan (PARSEC's own) | 1 / 2 / 4 | 30 each | **30 / 30 / 14** | PARSEC's |
| spdlog | compiler-tsan, one worker | 1, 2, 4, 8 logging | 30 each | 0 | every message once |
| spdlog | compiler-tsan, two workers | 8 logging | 30 | 0 | every message once |

(dedup's 60 are 30 on each input.)

- **No report from any implementation the compiler substituted**: none in
  dedup's 420 runs, which include the lock-free queue for one producer
  with up to four consumers and the single-consumer queue with up to
  eight producers, and none in spdlog's 150, whose single-consumer queue
  takes 160 000 messages from eight threads per run and delivers each
  once.
- **ferret's reports are PARSEC's own race, and only it.** Every one of
  the 24 in the port's builds and the 74 in PARSEC's is the same: the
  output stage reads PARSEC's progress counter `cnt_enqueue` for the line
  it prints to stderr while the loader increments it, both lines PARSEC's
  as written:

      WARNING: ThreadSanitizer: data race
        Read of size 4 ... by thread T10:
          #0 t_out(void*)
        Previous write of size 4 ... by thread T1:
          #0 file_helper(char const*)
          #1 scan_dir(char const*, char*)
          #2 scan_dir(char const*, char*)
          #3 t_load(void*)
        Location is global 'cnt_enqueue' of size 4

  Two of the reports are kept whole, their paths cut to the build's
  directory: `report-ferret-compiler-tsan-n2.txt` (the compiler's build,
  two threads per stage, no line numbers: the build goes through LLVM IR
  that carries none) and `report-ferret-orig-tsan-n1.txt` (PARSEC's own
  build, which names `ferret-pthreads.c:166`, the increment, and `:404`,
  the read).

  The port on the general queue reports it as often as the compiler's
  builds (10 of 90 runs against 11 of 90, and 3 of 30 with the inline
  rings), PARSEC's own build in every run at one and two threads per stage.
  How often it shows depends on whether the output stage happens to read
  the counter after an increment that no chain of hand-overs orders
  before the read, which the queues' timing changes and does not cause.
  The 21 October run, one run per configuration and no PARSEC build under
  ThreadSanitizer, did not meet it.
- **Every output is the original's**, the 98 ferret runs with a report
  included, which ran to their end.
- The instrumentation is live in this setup: besides PARSEC's own race,
  the negative controls of `test/Integration/tsan_own_build_caught.cpp`
  (a plain race on a global, and the ring forced on `queue_example.cpp`'s
  queue with two producers and two consumers), built with the same
  `wtc-cc --tsan` and run in the same container, each stop with a data
  race report (exit status 66).

A first pass the same day, three runs of each configuration with
`halt_on_error=1` as on 21 October, gave the same picture: one report, the
same `cnt_enqueue` race, in ferret's compiler build at two threads per
stage.

## How far it holds

- ThreadSanitizer sees the interleavings that happen; 30 runs on 24
  hardware threads exercise these queues' hand-overs millions of times
  (spdlog alone 30 × 160 000 messages through the single-consumer pop per
  configuration), but not every interleaving. Whether a pop in these runs
  passed a cell a push had taken and not yet filled is not observed; the
  runtime's deterministic tests drive that path (acac0fe), and those of the
  queue for one producer drive its claims (0208242).
- No AddressSanitizer run of the applications; the runtime's own tests run
  under it.
- The host was shared: other lanes' containers ran on the other socket and
  another of this project's jobs on part of this one. Nothing here is
  timed.

## Provenance

The tree was main at abe26c6 with this directory's branch's additions to
`benchmarks/apps/dedup/build.sh` (compiler-unity-tsan),
`benchmarks/apps/ferret/build.sh` (generic-tsan, orig-tsan) and
`benchmarks/scripts/tsan_apps.sh`, copied to the container host (not a git
checkout, so the builds' `build-info.txt` carries no hash); ClangIR at
1ebf89d8c5e6, PARSEC at 2e492f771c67, spdlog at 27cb4c767086. The run took
20 minutes (08:26 to 08:46 UTC).

    TSAN=1 benchmarks/apps/dedup/build.sh <parsec> <dedup-out>
    TSAN=1 benchmarks/apps/ferret/build.sh <parsec> <ferret-out>
    TSAN=1 benchmarks/apps/spdlog/build.sh <spdlog-src> <spdlog-out>
    REPS=30 benchmarks/scripts/tsan_apps.sh <dedup-out> <ferret-out> <spdlog-out> \
        <inputs>/simdev <inputs>/hamlet.dat <inputs>/native-32M.iso
