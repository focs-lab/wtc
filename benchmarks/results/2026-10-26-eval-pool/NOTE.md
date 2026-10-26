# eval-pool: a thread pool's tasks, 26 October 2026

`campaign.sh eval-pool` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors, 5 repetitions under
glibc, jemalloc and snmalloc (`WTC_ALLOCS`; tcmalloc and mimalloc were
left out to keep the leg under an hour): W11, one submitter and 1, 2, 4 or
8 workers, tasks whose capture is 16, 64 or 256 bytes and whose work is 0,
100 or 1000 ns (calibrated once, 1.025 rounds a ns, and passed to every
run as an exact count). Four variants, two factors:

- the queue: the general queue for many consumers (`mpmc`, the program
  as written) or the ring for one producer and many consumers (`spmc`,
  what the pool's topology allows the compiler to substitute);
- the task: a `std::function` whose capture the submitter allocates and a
  worker frees (`heap`, how a pool is written; at 16 bytes the capture
  fits the small buffer and nothing is allocated) or the function pointer
  and capture by value in the queue's cell (`cell`).

18:11 to 18:59 local; every run passed its check (each id run once), none
failed or timed out. Frequency not fixed: 2.13 to 4.12 GHz per run (median
4.11). The leg was not quiet: outside its set the processors were busy
0.254 of the time at the median, 0.598 at most (another lane's work on
socket 0), above the 0.10 a timed leg allows. The paired ratios do not
follow it (correlation of the log ratio with the difference in
outside-busy near zero, -0.004 for cell_spmc / heap_mpmc), and dropping
the repetitions above 0.2 moves no geometric mean by more than 0.015.
Control drift 3.1%, no trend.

## What it shows

Paired ratios, median per configuration, geometric mean over the 108
configurations (sizes × workers × work × allocators) with the range:

| step | all | work 0 | work 100 ns | work 1000 ns |
|---|---|---|---|---|
| both, cell_spmc / heap_mpmc | 1.56 (0.60-9.14) | 2.79 | 1.32 | 1.04 |
| topology on cell tasks, cell_spmc / cell_mpmc | 1.41 (0.42-9.17) | 2.47 | 1.17 | 0.97 |
| topology on heap tasks, heap_spmc / heap_mpmc | 1.07 (0.18-8.15) | 1.56 | 0.95 | 0.84 |
| task in the cell, general queue, cell_mpmc / heap_mpmc | 1.11 (0.50-4.88) | 1.13 | 1.14 | 1.07 |
| task in the cell, ring, cell_spmc / heap_spmc | 1.46 (0.65-6.19) | 1.79 | 1.40 | 1.23 |

- With no work the pool is the hand-over, and the two facts compound as
  in the ablation: 4.2× with one worker (9.1× at 16 bytes), 1.9-3.2× with
  more.
- The task in the cell pays where the capture is allocated: 1.04× at 16
  bytes on the general queue, where `std::function` keeps it inline, 1.28×
  at 256. The mechanism pass counts 1.09-1.10 allocations a task for the
  heap rows at 64 and 256 bytes, 0.002 for the cell rows (the queue's own
  segments).
- From 100 ns of work a task the queue is a smaller part of the cost:
  1.0-1.3× in the geometric mean, except at 8 workers, where the general
  queue's contention still shows at 100 ns (2.0-2.2×), and the loss below.

## How far it holds

- **The ring for many consumers loses with many workers and long tasks.**
  At 8 workers and 1000 ns of work the ideal is 8 M tasks a second; the
  medians over the nine size and allocator configurations are 6.8 for
  heap_mpmc and 4.0 for heap_spmc (topology 0.51×, 0.18-1.39), and on cell
  tasks 7.1 against 5.9 (0.85×, 0.74-0.97). The heap rows involve the
  allocator as well: under jemalloc at 64 and 256 bytes heap_spmc runs
  1.15-1.39 M tasks a second and the ratio falls to 0.18-0.20, against
  0.56-0.63 elsewhere (1.39 under glibc at 256 bytes). With the task in
  the cell as well, the ring is 1.05× (0.79-3.42) the pool as written
  there: the loss is the topology's alone, and the cell makes up for it.
  The likely cause, inferred from the design and not isolated: the ring's
  consumers take turns under one spin lock (`runtime/spmc_ring.hpp`), so
  tasks are handed out one lock transfer at a time, and where many
  workers come back at once that rate, not the work, is the limit. At 4
  workers and 100 ns the topology is also a loss, 0.72× on heap tasks and
  0.81× on cell tasks.
- Several cells are bimodal across repetitions (cell_mpmc at 4 workers and
  100 ns: 9.8 or 22.5 M/s), with every thread pinned to its own core;
  the cause is not isolated. Ratios are taken within a repetition, and the
  medians stand.
- A microbenchmark: one submitter, tasks of fixed size and work, no
  results returned. A pool in a program adds what its tasks do.

## What it means for the compiler

The topology a pool proves is worth taking when tasks are short (2-4× at
no work) and is a loss with many workers and long tasks on the current
ring for many consumers. Before the compiler substitutes it by default it
needs either an implementation whose consumers do not take turns, or an
estimate of the work a task does, as coarsening does (W8).

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. The runtime, shim and W11 are those
of main from a19428f to 7b40685 (compared file by file); driven by
campaign.sh at a19428f, which writes `unpinned.tsv` with a header (run
number, the first and last `runs.jsonl` line the run wrote, user GHz, the
ratio to reference cycles, nan on this host, user cycles, task-clock,
outside-busy share, allocator, command).
