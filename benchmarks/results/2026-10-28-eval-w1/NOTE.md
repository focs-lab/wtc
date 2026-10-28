# eval-w1 on the current runtime: one producer, one consumer, 28 October 2026

The leg of `2026-10-26-eval-w1` repeated on main at abe26c6. Since that
leg the ring's consumer waits for a batch only while the producer streams
(ffaa22d), the general-purpose queue names the hazard slot each operation
uses (0208242) and the single consumer's pop and the queue for one
producer changed (acac0fe, 0208242), which W1 does not run. The shim's
tags and the workload are unchanged.

`campaign.sh eval-w1` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors (two cores), 5 repetitions
under glibc, jemalloc, tcmalloc, mimalloc and snmalloc, every W1 variant at
8, 64 and 256 bytes, then the mechanism pass; the CMake build rebuilt
there from the same tree. 17:18 to 17:56 local (09:18 UTC). oneTBB is not
installed on this host, so onetbb_cq_sp is absent.

Frequency not fixed: 1.93 to 4.12 GHz per run (median 4.10); outside-busy
median 0.001, at most 0.045, as on 26 October. Control drift 9.2%: the
opening controls read low (14.6 against 16.1 M/s at the close) while
the repetitions are flat (rep 0 to 4 within 0.5%), a warm-up that ended
before the first configuration, not drift.

## What it shows

Paired ratios, geometric means over sizes and allocators (range), beside
the 26 October leg's:

| ratio | 28 Oct | range | by size (8 / 64 / 256 B) | 26 Oct | range |
|---|---|---|---|---|---|
| compiler's choice (shim_spsc_inline) / as written (shim_generic) | 11.3 | 5.1-27.2 | 24.7 / 10.2 / 5.6 | 10.2 | 5.1-22.7 |
| the topology alone: shim_spsc / as written | 6.56 | 4.51-8.72 | 6.43 / 6.72 / 6.54 | 5.90 | 1.66-8.69 |
| the borrow proof: shim_spsc_inline / shim_spsc | 1.72 | 0.70-5.03 | 3.93 / 1.51 / 0.86 | 1.73 | 0.89-4.49 |
| shim_spsc_move / as written | 10.9 | 4.9-24.9 | 23.2 / 10.5 / 5.3 | 10.0 | 4.7-22.9 |
| compiler's choice / moodycamel_cq_sp (best general-purpose library of those measured) | 10.7 | 3.2-37.0 | 23.6 / 9.5 / 5.4 | 10.3 | 3.2-31.1 |
| compiler's choice / rigtorp_inline (expert SPSC) | 0.84 | 0.49-1.30 | 0.64 / 0.99 / 0.93 | 0.87 | 0.62-1.43 |
| compiler's choice / moodycamel_rw_inline (expert SPSC) | 2.29 | 1.74-3.25 | 3.12 / 2.20 / 1.76 | 2.23 | 1.73-2.98 |
| as written / orig_generic (the retired two-lock list) | 2.65 | 1.94-4.61 | | 2.80 | 1.88-12.0 |
| as written / moodycamel_cq_sp | 0.94 | 0.55-2.18 | | 1.00 | 0.56-4.19 |

Per size, the geometric mean over the five allocators of each
allocator's median (min-max), M elements/s:

| | 8 B | 64 B | 256 B |
|---|---|---|---|
| as written (shim_generic) | 17.2 (16.4-19.2) | 15.4 (15.1-15.9) | 13.1 (12.5-14.3) |
| moodycamel_cq_sp | 18.5 (11.9-28.0) | 16.6 (11.2-25.5) | 13.7 (6.5-23.0) |
| moodycamel_rw_inline | 138.7 (133.4-151.2) | 71.7 (71.0-72.6) | 42.0 (41.2-42.6) |
| rigtorp_inline | 628.7 (578.9-701.7) | 159.0 (121.7-172.8) | 79.0 (67.0-84.2) |
| shim_spsc (the topology alone) | 110.9 (87.2-140.1) | 105.1 (84.0-137.0) | 85.7 (65.2-104.8) |
| shim_spsc_inline (the compiler's choice) | 433.2 (421.9-446.3) | 157.8 (156.6-160.1) | 73.8 (73.3-74.3) |
| ring_inline (the bare ring, element in the cell) | 870.9 (842.0-900.2) | 323.2 (313.9-328.2) | 93.9 (87.9-96.8) |

- **The headline holds and grows**: the compiler's choice is 11.3× the
  program as written (10.2× on 26 October), 0.84× rigtorp's ring (0.87×),
  2.3× moodycamel's ReaderWriterQueue. Without the one configuration that
  moves most between the days (below) it is 10.8× against 10.4×.
- **The topology alone is 6.6× (5.9×), the borrow proof 1.7× on top
  (1.7×)**, and 6.6 × 1.7 ≈ 11. The borrow proof is still a loss at
  256 bytes (0.86×; 0.70-0.71× under snmalloc and tcmalloc) and worth 3.9×
  at 8.
- **What moved between the days.** As written at 8 bytes under glibc is
  bimodal: 50 M/s in four of the five repetitions on 26 October and in one
  on 28 October, 16.5 to 21.1 otherwise; the cause is not measured
  (shim_spsc makes the same shared_ptr pop at 8 bytes under glibc and runs
  in one mode, 86 to 87 M/s, and the mechanism pass, with the same
  allocations per element, runs as written at 29.5). Its median went
  from 50.0 to 19.2, which turns that configuration's topology ratio from
  1.66× into 4.51× and its headline from 7.4× into 21×; every other
  allocator and
  size of as written is within 5% of 26 October. The compiler's choice at
  8 bytes runs 1.16× its 26 October rate (1.12-1.19 across allocators),
  level at 64 and 256; rigtorp's ring runs 1.12× at 8 bytes on the same
  comparison, so the across-days part is indicative only (another day,
  the frequency not fixed); within the leg, the ratios are paired.
- Moving the element out (try_pop(T&), no proof needed) runs at 0.96× of
  lending (0.99× on 26 October).

## How far it holds

- Ratios that involve as written at 8 bytes depend on the allocator and,
  under glibc, on which of its two modes a run falls in; the table's
  ranges cover all five allocators.
- Frequency is not fixed; absolute rates are not compared with other
  hosts, the ratios are.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of main at
abe26c6 on the second host, not a git checkout, driven by campaign.sh at
abe26c6, whose `unpinned.tsv` has a header: run number, the first and
last `runs.jsonl` line the run wrote ("-" for none), user GHz, the ratio
to reference cycles (nan on this host), user cycles, task-clock,
outside-busy share, allocator, command.
