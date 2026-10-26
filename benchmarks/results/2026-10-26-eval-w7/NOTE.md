# eval-w7: batching a producer's pushes, 26 October 2026

`campaign.sh eval-w7` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors (two cores), 5 repetitions
under glibc and jemalloc, every flavour of build_w7_flavours.sh (main
5a70189): plain, batched by the compiler and batched by hand, on the
general-purpose queue, the spsc ring and the ring with the element in the
cell, 8- and 64-byte elements, batch sizes 1 to 1024; closed loop, then
paced at one element a microsecond. 15:26 to 15:45 local.

Frequency not fixed: 2.99 to 4.12 GHz per run (median 3.92); outside-busy
median 0.000, at most 0.053. Control drift 1.4%, no trend.

## What it shows

**Batching pushes gains nothing on the current queues.** Batched over
plain is 1.00 at every batch size, both element sizes, both allocators, on
all three queues; the compiler's batch equals the hand batch (1.00).
Medians, 64 B, glibc, M elements/s: general-purpose 23.17 plain, 23.21 at
K = 64; spsc 28.91 and 28.97; element in the cell 42.11 and 42.08.

Its price is delay: paced at 1 M elements a second, the median wait is
0.1 to 0.2 µs unbatched, 2.1 µs at K = 4, 8.4 at 16, 33 at 64, 133 at 256
and about 530 at 1024, the same on every queue (an element waits for its
batch to fill).

## How far it holds

- The 21 October leg found 1.8× at K = 64 on the general-purpose queue:
  that queue was the two-lock list, whose push took a mutex. The segmented
  queue reserves a batch of cells with one atomic, and the rings publish
  with one store, so there is nothing left to amortise.
- Batching stays legal (the analysis proves when it is), and may still pay
  on a queue whose push locks; none of the runtime's queues does now.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. The binaries were built from main at
5a70189 (the runtime, shim and workloads are unchanged from there to
3718285), driven by campaign.sh at 576564e. `unpinned.tsv` here has the
earlier six-column form with no header: the first and last `runs.jsonl`
line the run wrote (first greater than last: none), the effective
frequency in GHz, the busy share of the processors outside the leg's set,
the allocator and the command. Legs driven from a19428f on write the
headed form with a run number, user cycles and task-clock as well.
