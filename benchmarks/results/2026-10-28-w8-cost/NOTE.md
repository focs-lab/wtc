# w8-cost: the coarsening cost model on and off, 28 October 2026

An untimed A/B on the container host (Intel Xeon Gold 6230, node 1,
processors 28 to 35, one per thread on distinct cores, their SMT siblings
idle; governor `powersave` with turbo, so frequency not fixed). It checks
the cost model of `decisions/0006`: does `--wtc-batch-locks` refuse to hold
the mutex across sections where that loses, and keep doing it where it
wins? It also gives the host's own crossovers, from which the
`cascade-lake` class of the model is set.

W8 reads its work at run time, which the model cannot bound, so the leg
uses `build_w8_flavours.sh` in its cost mode: the work fixed when the
program is built (`-DW8_WORK_ROUNDS`) at round(0.32 L) rounds for the
levels L = 0, 20, 50, 100, 200 and 500 ns (0, 6, 16, 32, 64 and 160
rounds; 0.32 rounds a ns is the median of the host's earlier runs), each
built three ways:

- `plain`: the loop as written, one lock and unlock a section;
- `off`: marked `WTC_BATCH`, the model off (`--batch-cost off`): batched
  wherever legal, as before the model;
- `on`: marked `WTC_BATCH`, the model on with the default class's
  thresholds and this host's constant (`--batch-ns-per-unit 0.125`, one
  round in 3.125 ns). W8 starts its threads in a loop of a count read at
  run time, so they are not counted and the lowest threshold, 180 ns,
  holds. The model batched at 0 to 100 ns and refused at 200 and 500
  (`decisions.txt`).

Twelve repetitions at 1 M sections a thread, 1, 2, 4 and 8 threads; within
each repetition the configurations in a fresh random order and the three
builds of each back to back in a fresh random order (`run.sh`; each
repetition held the build lock and started only when nothing else held
processors 28 to 39 or 68 to 79). Every run's line has the UTC times it
started and ended. Every 5 s a monitor recorded what else held those
processors (`monitor.log`: nothing, from 10:13:42 to 10:29:28 by the host's
clock, which runs about 27 minutes behind) and what held the rest of node 1
(`monitor-node1.log`): another lane's DPDK audit, a build, on processors
20 to 27 throughout, so the leg shared the socket's cache and memory
bandwidth with it. `ab.py` leaves out any run with something on the leg's
processors within 5 s of it; there was none. All runs of a thread count
printed the same checksum.

An earlier run of this leg, on processors 20 to 27, overlapped other jobs
on those processors and is not kept.

## What it shows

Where the model batched, its build is the `off` program, and where it
refused, the `plain` one, so each repetition's batched and plain rates
pool the two runs of their program (`ab.py`). Medians over the twelve
repetitions, range in brackets; the level's measured work is what its
rounds took by each run's own calibration.

| threads | level, ns (measured) | model | batched / plain | model on / off |
|---|---|---|---|---|
| 1 | 0 (0) | batched | 4.13 (3.16-6.68) | 1.00 |
| 1 | 20 (17) | batched | 1.31 (1.23-1.95) | 0.99 |
| 1 | 50 (49) | batched | 1.05 (0.82-1.12) | 1.01 |
| 1 | 100 (92) | batched | 1.02 (0.84-1.11) | 0.99 |
| 1 | 200 (198) | refused | 1.05 (1.02-1.11) | 0.95 |
| 1 | 500 (489) | refused | 1.04 (0.96-1.08) | 0.96 |
| 2 | 0 (0) | batched | 6.25 (5.63-9.02) | 0.94 |
| 2 | 20 (15) | batched | 3.66 (3.09-4.00) | 1.00 |
| 2 | 50 (50) | batched | 2.41 (1.94-2.85) | 1.01 |
| 2 | 100 (98) | batched | 1.67 (1.58-1.89) | 0.99 |
| 2 | 200 (198) | refused | 0.86 (0.71-1.00) | 1.15 |
| 2 | 500 (401) | refused | 0.58 (0.54-0.66) | 1.71 |
| 4 | 0 (0) | batched | 9.52 (8.38-11.06) | 1.00 |
| 4 | 20 (17) | batched | 3.83 (3.62-4.35) | 0.98 |
| 4 | 50 (40) | batched | 0.93 (0.84-1.98) | 1.01 |
| 4 | 100 (100) | batched | 0.77 (0.69-0.89) | 0.99 |
| 4 | 200 (193) | refused | 0.72 (0.63-0.83) | 1.42 |
| 4 | 500 (497) | refused | 0.47 (0.39-0.50) | 2.16 |
| 8 | 0 (0) | batched | 9.84 (9.35-10.96) | 0.99 |
| 8 | 20 (19) | batched | 1.66 (1.48-1.84) | 0.99 |
| 8 | 50 (49) | batched | 0.95 (0.91-1.00) | 1.01 |
| 8 | 100 (100) | batched | 0.80 (0.72-1.04) | 1.00 |
| 8 | 200 (198) | refused | 0.81 (0.72-0.96) | 1.23 |
| 8 | 500 (493) | refused | 0.67 (0.64-0.70) | 1.52 |

- **Where the model refused, it avoided the loss.** At 200 and 500 ns
  batching lost at every thread count above one (0.47 to 0.86), and the
  model's build ran 1.15 to 2.16 times the batched one there. With one
  thread batching never loses, and refusing it gave up 4 to 5% (0.95,
  0.96).
- **Where it batched, it kept the gain** with one and two threads at every
  level (6.25, 3.66, 2.41 and 1.67 times the plain loop at 0 to 100 ns
  with two), and with four and eight at 0 and 20 ns (9.5 to 9.8; 3.83 and
  1.66).
- **With four and eight threads this host loses from about 50 ns.**
  Batching was at 0.93 and 0.95 of the plain loop at 50 ns (four
  threads' range 0.84 to 1.98: not clearly a loss) and 0.77 and 0.80 at
  100 ns, where the timed leg's host had it winning up to 200 ns. The
  model, with that host's thresholds, batched here, at that cost. The
  batched loop stayed fair here (Jain's index 0.99 to 1.00 at 50 and
  100 ns with four and eight threads, against 0.59 to 0.75 on the other
  host), so the mutex went to a waiting thread at nearly every release;
  that it is the hand-over that costs is a reading, not a measurement.
- **This host's crossovers**, by the method of `decisions/0006` (the
  logarithm of the speed-up interpolated linearly in the work between the
  two levels around 1): 177 ns with two threads, 48 with four, 47 with
  eight; the four- and eight-thread ones lie between 20 and 50 ns, and
  where in that interval is uncertain (the 50 ns ratios are 0.93 and 0.95
  with ranges reaching 1). A round took 0.327 rounds a ns by the median of
  the 720 runs with work (0.307 to 0.454 between the 10th and 90th
  percentiles), 0.122 ns a unit. W8's loop with no work is 212 units,
  26 ns. Hence the `cascade-lake` class: 0.122 ns a unit, thresholds
  177 + 26 and 47 + 26, rounded down to 200 and 70 ns.
- **With that class** W8's threads are still not counted, so 70 ns holds:
  the model batches at 0 and 20 ns and refuses from 50 on
  (`decisions.txt`). Read off the same runs, that keeps 4.1 to 9.8 times at
  no work and 1.31 to 3.83 at 20 ns, never batches at a loss, and gives up
  2.41 and 1.67 at 50 and 100 ns with two threads (and 5% with one), which
  only a count of two threads would keep: what an uncounted thread count
  costs here.
- **On the timed leg's host**, at that leg's rounds (`2026-10-21-eval-w8`,
  0.2697 rounds a ns), the default class batches at 0 to 100 ns and
  refuses at 200 and 500 (`decisions.txt`): by that leg's medians it keeps
  every gain but the 1.20 and 1.33 at 200 ns with four and eight threads,
  and avoids every loss.

## How far it holds

- Untimed, with the frequency not fixed and another lane's build on the
  same socket; the ratios within a repetition are what the leg can carry.
  The ranges are wide where the ratio is near 1.
- One workload, whose work is one dependent chain, at K = 64.
- The thresholds of the `cascade-lake` class come from this leg; the check
  of that class on this host is the same runs read with its decisions,
  not an independent measurement.

## Provenance

Built on the container host from a copy of the tree at 9db67a257888
(`build-info.txt`): this branch's commit that lets W8 fix its work, as it
stood before the branch was rebased onto main at 6a85f29, which brought
the model of DPDK's mempools and documentation and nothing these builds
use. The rebased pass makes the same decision, with the same estimate, at
every level and in every section of `decisions.txt`. The builds ran with
`W8_COST_LEVELS="0 20 50 100 200 500" W8_ROUNDS_PER_NS=0.32
W8_NS_PER_UNIT=0.125`. The two later sections of `decisions.txt` come from
the same pass with the options they name. `ab.py runs.jsonl decisions.txt
monitor.log` recomputes every ratio here; `run.sh` is one repetition, run
as `flock <build lock> run.sh <build dir> runs.jsonl <rep>` in the
container with `W8_CPUS=28,29,30,31,32,33,34,35`, for repetitions 0 to 11.
