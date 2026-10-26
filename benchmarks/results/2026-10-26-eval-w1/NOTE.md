# eval-w1: one producer, one consumer, 26 October 2026

`campaign.sh eval-w1` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors (two cores), 5 repetitions
under glibc, jemalloc, tcmalloc, mimalloc and snmalloc, every W1 variant at
8, 64 and 256 bytes, then the mechanism pass; the CMake build at main
5a70189, driven by campaign.sh at a19428f. 16:20 to 16:59 local. oneTBB is
not installed on this host, so onetbb_cq_sp is absent.

Frequency not fixed: 2.13 to 4.12 GHz per run (median 4.09); outside-busy
median 0.001, at most 0.046. Control drift 2.2%, no trend.

## What it shows

Paired ratios, geometric means over sizes and allocators (range):

| ratio | gm | range |
|---|---|---|
| compiler's choice (shim_spsc_inline) / as written (shim_generic) | 10.2 | 5.1-22.7 |
| shim_spsc_move / as written | 10.0 | 4.7-22.9 |
| compiler's choice / moodycamel_cq_sp (best general-purpose library of those measured) | 10.3 | 3.2-31.1 |
| compiler's choice / rigtorp_inline (expert SPSC) | 0.87 | 0.62-1.43 |
| compiler's choice / moodycamel_rw_inline (expert SPSC) | 2.23 | 1.73-2.98 |
| as written / orig_generic (the retired two-lock list) | 2.80 | 1.88-12.0 |
| as written / moodycamel_cq_sp | 1.00 | 0.56-4.19 |

Medians, glibc, M elements/s: as written 50.0 / 15.1 / 14.7 at 8 / 64 /
256 B; the compiler's choice 369.7 / 165.4 / 74.0; rigtorp_inline 613.9 /
115.5 / 66.7; the bare ring with the element in the cell 863 / 300 / 81.

So the general-purpose queue the program gets without proof is now level
with the best library measured, and the compiler's choice is within 13% of
the expert rings: 0.87× rigtorp's in the geometric mean, behind at 8 B
(0.62-0.71 across allocators) and within about 10% at 64 and 256 B, ahead
under glibc only. Moving the element out (try_pop(T&), no proof needed)
runs as fast as lending it.

## How far it holds

- The 21 October timed leg measured the same variants against the retired
  two-lock list (17-60× over as written); this leg supersedes its ratios.
- As written at 8 B is 50 M/s under glibc but 17 under jemalloc and
  snmalloc: the shared_ptr the consumer allocates is cheapest in glibc's
  thread cache. Ratios that involve as written depend on the allocator for
  that reason; the table's ranges cover all five.
- Frequency is not fixed; absolute numbers are not comparable with the
  21 October leg on the Xeon host, the ratios are.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. The binaries were built from main at
5a70189 (the runtime, shim and workloads are unchanged from there to
3718285), driven by campaign.sh at a19428f, which writes `unpinned.tsv`
with a header: run number, the first and last `runs.jsonl` line the run
wrote ("-" for none), user GHz, the ratio to reference cycles (nan on this
host), user cycles, task-clock, outside-busy share, allocator, command.
