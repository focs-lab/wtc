# eval-ablation: what topology and ownership buy, 26 October 2026

`campaign.sh eval-ablation` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors, 5 repetitions under
glibc, jemalloc, tcmalloc, mimalloc and snmalloc; W10's eight rows at 8,
64 and 256 bytes, on separate cores and on SMT siblings (the CMake build
at main 5a70189). 13:37 to 14:11 local.

Frequency not fixed: 2.15 to 4.12 GHz per run (median 4.10); outside the
leg's set the processors were busy 0.195 of the time at the median, 0.574
at most (another lane's work on socket 0). Ratios are taken within a
repetition and are tight (spsc_cell over mc_cell at 64 B is 8.3×, 7.8 to
9.0 across allocators). Control drift 0.6%, no trend.

## The rows

mpmc and spsc are the runtime's segmented queue and ring; mc is
moodycamel's ConcurrentQueue, the strongest general-purpose library
measured. xsp: the element behind a shared_ptr the producer allocates;
share: the element in the cell, handed out as a shared_ptr the consumer
allocates (what the shim does unless something is proved: the program as
written); cell: the element in the cell, moved out or lent.

Medians, separate cores, M elements/s, glibc (the other allocators in
SUMMARY.md):

| | mpmc_xsp | mpmc_share | mpmc_cell | mc_xsp | mc_cell | spsc_xsp | spsc_share | spsc_cell |
|---|---|---|---|---|---|---|---|---|
| 8 B | 7.3 | 31.0 | 16.3 | 12.0 | 37.0 | 17.8 | 120.9 | 887.9 |
| 64 B | 7.6 | 16.6 | 16.1 | 11.6 | 34.1 | 16.9 | 111.6 | 271.4 |
| 256 B | 6.2 | 16.6 | 13.6 | 6.7 | 28.4 | 9.1 | 97.7 | 87.6 |

## What it shows

Geometric means over sizes and allocators, separate cores (range):

- The compiler's own step over the program as written through the shim,
  spsc_cell / mpmc_share: 17.0× (5.3-54); by size 47 / 17 / 6.1. It splits
  into the topology, spsc_share / mpmc_share, 8.3× (3.9-14.2), and the
  borrow proof, spsc_cell / spsc_share, 2.0× (0.82-7.35; a loss at 256 B).
- Against a strong library that already holds the element by value,
  spsc_cell / mc_cell: 8.8× (3.1-28); 26× at 8 B, 8× at 64 B, 3× at 256 B.
  The topology matters when nothing is allocated.
- The full diagonal, spsc_cell / mpmc_xsp (a heap-allocating MPMC): 24×
  (5.2-120).
- The element in the cell alone: 7.5× on the ring, 1.3× on the segmented
  queue; on moodycamel 1.9×.
- Against the heap row spsc_xsp under its best allocator (snmalloc),
  spsc_cell is 5.9× at 8 B, 2.0× at 64 B and level at 256 B.
- On SMT siblings (one core, shared caches) spsc_cell is 13.5× over
  mc_cell and 15.3× over mpmc_xsp: the heap row gains from the shared
  cache, the cell row less.

## How far it holds

- Microbenchmark: one producer, one consumer, nothing but the hand-over.
  What a program gains is bounded by its grain (eval-w3grain).
- The runtime itself now holds every element by value, so much of the
  "element in the cell" step is the runtime's, not the proof's: the proof
  only chooses lending over the shared_ptr form, and moving the element
  out (try_pop(T&)) needs no proof at all.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. The binaries were built from main at
5a70189 (the runtime, shim and workloads are unchanged from there to
3718285), driven by campaign.sh at 5a70189. `unpinned.tsv` here has the
earlier six-column form with no header: the first and last `runs.jsonl`
line the run wrote (first greater than last: none), the effective
frequency in GHz, the busy share of the processors outside the leg's set,
the allocator and the command. Legs driven from a19428f on write the
headed form with a run number, user cycles and task-clock as well.
