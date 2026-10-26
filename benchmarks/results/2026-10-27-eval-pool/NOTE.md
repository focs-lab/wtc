# eval-pool: a thread pool's tasks on the lock-free spmc queue, 27 October 2026

The 26 October leg (`2026-10-26-eval-pool`) run again on main at e52c445,
where the queue for one producer and many consumers is the lock-free
`runtime/spmc_queue.hpp` (consumers claim a cell with a compare-and-swap
and move the element out holding nothing) instead of the ring whose
consumers took turns under one spin lock. Same leg otherwise:
`campaign.sh eval-pool` with `WTC_UNPINNED=1` on the two-socket AMD EPYC
9115, socket 1's processors, 5 repetitions under glibc, jemalloc and
snmalloc; W11, one submitter and 1, 2, 4 or 8 workers, captures of 16, 64
or 256 bytes, 0, 100 or 1000 ns of work a task (calibrated 1.025 rounds a
ns). The variants are as on 26 October: the queue general (`mpmc`, the
program as written) or for one producer (`spmc`, what the pool's topology
lets the compiler substitute); the task a `std::function` whose capture
the submitter allocates (`heap`) or held by value in the cell (`cell`).

27 October 01:35 to 02:23 local (26 October 17:35 to 18:23 UTC). The
campaign names its directory by the UTC date, so on the second host this
leg wrote into `2026-10-26-*` beside the earlier leg; the directory here
carries the local date instead, and `meta.json` and the recorded commands
keep the UTC one. The leg appended to that directory's log and checksum
files; the copies here keep only this leg's lines. Every run passed its
check. Frequency not fixed: user clock median 4.04 GHz, 1.64 to 4.12.
Outside the leg's set the processors were busy 0.002 of the time at the
median, 0.50 at most, above the 0.10 gate in 773 of 2,193 runs (other work
on socket 0). `SUMMARY.md`'s verdict is "drift 12.9%, NOISY: rerun this
leg" and "drifting in one direction": the control read 13.9 Mops/s at the
start and 16.0 at the end, and the absolute rates rose by 18% from the
first repetition to the last. The ratios below are taken within a
repetition, and both sides of every pair moved together: the
per-repetition geometric means stay within 2.46-2.54 (both factors),
2.12-2.25 (topology on cell tasks) and 1.65-1.70 (on heap tasks), and
keeping only pairs whose runs were both under the gate changes them by at
most 0.02. The absolute rates quoted below carry the drift; the ratios do
not. The analysis leaves out the leg's 24 mechanism-pass runs (`"pass":
"mechanism"`, the allocation counter) and the control runs (repetitions
900 and 901).

## What it shows

Paired ratios, median per configuration, geometric mean over the 108
configurations with the range; the 26 October figure, on the turn-taking
ring, in brackets:

| step | all | work 0 | work 100 ns | work 1000 ns |
|---|---|---|---|---|
| both, cell_spmc / heap_mpmc | 2.52 (0.81-26.4) [1.56] | 5.45 [2.79] | 2.70 [1.32] | 1.09 [1.04] |
| topology on cell tasks, cell_spmc / cell_mpmc | 2.25 (0.99-19.4) [1.41] | 4.84 [2.47] | 2.34 [1.17] | 1.01 [0.97] |
| topology on heap tasks, heap_spmc / heap_mpmc | 1.70 (0.89-9.65) [1.07] | 2.60 [1.56] | 1.89 [0.95] | 1.00 [0.84] |
| task in the cell, general queue, cell_mpmc / heap_mpmc | 1.12 (0.50-5.35) [1.11] | 1.13 | 1.16 | 1.08 |
| task in the cell, spmc, cell_spmc / heap_spmc | 1.47 (0.68-26.6) [1.46] | 2.08 | 1.42 | 1.09 |

- **The loss is gone.** At 8 workers and 1000 ns of work the topology is
  1.00× on cell tasks (0.99-1.02) and 1.00× on heap tasks (0.98-1.02),
  where the turn-taking ring gave 0.85× and 0.51×; the medians of the 45
  runs there (three sizes, three allocators, five repetitions, pooled)
  are 7.47 against 7.49 M tasks a second (cell) and 6.93 against 6.71
  (heap), against an ideal of 8. At 4
  workers and 100 ns, where the ring gave 0.81× and 0.72×, the topology now
  gives 2.34× and 1.92×.
- No configuration of the topology on cell tasks is below 0.99, one on
  heap tasks is below 0.95 (8 workers, 100 ns, 256 bytes, glibc: 0.89×).
- With no work the pool is the hand-over, and the two facts compound:
  5.45× over the pool as written.
- The task in the cell changes little between the two days: allocations a
  task are 1.10 for the heap rows at 64 and 256 bytes and 0.002 for the
  cell rows (the queue's own segments), as before.

## How far it holds

- Microbenchmark: one submitter, tasks that do nothing but spin for their
  work. Frequency not fixed and the leg not quiet against the 0.10 gate;
  the paired design and the checks above are what the ratios rest on.
- The 26 October leg stays as the record of the turn-taking ring and of
  the loss that led to replacing it.

## Provenance

The tree on the second host was synced from main at e52c445 (not a git
checkout, so `meta.json`'s `git_sha` is empty) and its benchmarks rebuilt
there (clang++-18, Release, native). Chain and protocol as on 26 October.
