# eval-ablation on the current runtime: what topology and ownership buy, 28 October 2026

The leg of `2026-10-26-eval-ablation` repeated on main at abe26c6. Since
that leg the ring's consumer waits for a batch only while the producer
streams (ffaa22d) and the segmented queue names the hazard slot each
operation uses (0208242); its single consumer's pop changed too
(acac0fe), which no row here runs (`mpmc_*` is the many-consumer form).
W10 and the leg are unchanged.

`campaign.sh eval-ablation` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors, 5 repetitions under
glibc, jemalloc, tcmalloc, mimalloc and snmalloc; W10's eight rows at 8,
64 and 256 bytes, on separate cores and on SMT siblings (the CMake build
rebuilt there from the same tree). 17:56 to 18:30 local (09:56 UTC).

Frequency not fixed: 2.15 to 4.12 GHz per run (median 4.10). Outside the
leg's set the processors were busy 0.26 of the time at the median, 0.64
at most (other work on socket 0; 0.195 and 0.574 on 26 October), so the
leg was not quiet and only paired ratios are reported. Control drift
0.2%, no trend across repetitions.

## The rows

As on 26 October: mpmc and spsc are the runtime's segmented queue and
ring; mc is moodycamel's ConcurrentQueue. xsp: the element behind a
shared_ptr the producer allocates; share: the element in the cell,
handed out as a shared_ptr the consumer allocates (the shim's `try_pop()`
unless something is proved: the program as written); cell: the element in
the cell, moved out (`try_pop(T&)`).

Medians, separate cores, M elements/s, glibc (the other allocators in
SUMMARY.md):

| | mpmc_xsp | mpmc_share | mpmc_cell | mc_xsp | mc_cell | spsc_xsp | spsc_share | spsc_cell |
|---|---|---|---|---|---|---|---|---|
| 8 B | 7.4 | 19.5 | 16.3 | 12.1 | 36.9 | 18.2 | 113.9 | 768.8 |
| 64 B | 7.5 | 16.8 | 15.8 | 11.6 | 34.3 | 17.1 | 118.5 | 280.7 |
| 256 B | 6.1 | 16.5 | 14.0 | 6.7 | 28.6 | 9.4 | 94.7 | 87.0 |

Per size, the geometric mean over the five allocators of each
allocator's median (min-max), separate cores:

| | 8 B | 64 B | 256 B |
|---|---|---|---|
| mpmc_share (the general queue, shared_ptr pop) | 17.0 (16.2-19.5) | 16.1 (15.6-16.8) | 14.8 (14.2-16.5) |
| mc_cell (moodycamel by value) | 33.9 (30.6-37.7) | 33.5 (31.2-35.6) | 28.3 (27.8-29.0) |
| spsc_share (the topology) | 164.8 (113.9-213.7) | 140.8 (118.5-158.9) | 105.1 (94.7-119.2) |
| spsc_cell (topology and pop form) | 851.5 (768.8-881.1) | 291.3 (280.7-302.2) | 92.8 (87.0-98.6) |

## What it shows

Geometric means over sizes and allocators, separate cores (range), beside
the 26 October leg's:

| ratio | 28 Oct | range | by size (8 / 64 / 256 B) | 26 Oct | range |
|---|---|---|---|---|---|
| spsc_cell / mpmc_share: both facts | 17.9 | 5.3-54 | 50 / 18 / 6.3 | 17.0 | 5.3-54 |
| spsc_share / mpmc_share: the topology | 8.4 | 5.7-12.8 | 9.7 / 8.7 / 7.1 | 8.3 | 3.9-14.2 |
| spsc_cell / spsc_share: the pop form | 2.13 | 0.82-6.74 | 5.2 / 2.1 / 0.90 | 2.04 | 0.82-7.35 |
| spsc_cell / mc_cell: over a library by value | 9.0 | 3.1-30 | 25 / 8.8 / 3.3 | 8.8 | 3.1-28 |
| spsc_cell / mpmc_xsp: the diagonal | 24.5 | 5.5-105 | 73 / 25 / 8.2 | 23.7 | 5.2-120 |
| spsc_cell / spsc_xsp: the cell alone, ring | 7.7 | 1.05-42 | 19 / 7.0 / 3.4 | 7.5 | 1.01-50 |
| mpmc_cell / mpmc_xsp: the cell alone, segmented queue | 1.32 | 0.85-2.24 | | 1.31 | 0.82-2.20 |
| mc_cell / mc_xsp: the cell alone, moodycamel | 1.89 | 1.11-4.25 | | 1.92 | 1.12-4.23 |

- **The decomposition holds on the current runtime**: 17.9× from the
  general queue to the single-producer ring with the element moved out,
  the topology 8.4× and the pop form 2.1× of it (17.0×, 8.3× and 2.0× on
  26 October). The pop form is still a loss at 256 bytes (0.90×,
  0.82-0.97 across allocators); moving the element out needs no proof.
  What the borrow proof buys through the shim is measured in eval-w1:
  shim_spsc_inline / shim_spsc, 1.7× (`2026-10-28-eval-w1`).
- Against a strong library that already holds the element by value,
  spsc_cell / mc_cell: 9.0× (25× at 8 B, 8.8× at 64 B, 3.3× at 256 B).
- The full diagonal against a heap-allocating MPMC: 24×. Against the heap
  row spsc_xsp under its best allocator (snmalloc), spsc_cell is 5.9× at
  8 B, 2.2× at 64 B and level at 256 B.
- The element in the cell alone: 7.7× on the ring, 1.3× on the segmented
  queue, 1.9× on moodycamel.
- On SMT siblings spsc_cell is 13.3× over mc_cell and 15.6× over
  mpmc_xsp (13.5× and 15.3×); topology 2.3×, pop form 4.6×.
- **What moved between the days.** The topology's lowest configuration
  was 3.9× (8 bytes, glibc) on 26 October and is 5.65× (256 bytes, glibc)
  now; at 8 bytes under glibc it is 5.84×, because the general queue's
  shared_ptr pop there varies from run to run: 31.0 M/s at the median on
  26 October, 19.5 now. Every
  other row is within 3% of 26 October on the geometric mean across
  days (indicative: another day, the frequency not fixed); spsc_cell at
  8 bytes under glibc reads 769 against 888, the other allocators 863 to
  881 against 881 to 887.

## How far it holds

- Microbenchmark: one producer, one consumer, nothing but the hand-over.
  What a program gains is bounded by its grain (eval-w3grain).
- The leg was not quiet (above); the ratios are taken within a
  repetition, and the decomposition's numbers moved by 1-5% from
  26 October.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of main at
abe26c6 on the second host, not a git checkout, driven by campaign.sh at
abe26c6, whose `unpinned.tsv` has a header: run number, the first and
last `runs.jsonl` line the run wrote ("-" for none), user GHz, the ratio
to reference cycles (nan on this host), user cycles, task-clock,
outside-busy share, allocator, command.
