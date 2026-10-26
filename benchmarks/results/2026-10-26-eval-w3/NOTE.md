# eval-w3: a three-stage pipeline, 26 October 2026

`campaign.sh eval-w3` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors (three cores), 5
repetitions under glibc and jemalloc: the adapter pipeline with every
baseline class, and the compiler's pipeline in its four builds
(build_wtc_flavours.sh at main 5a70189: generic = specialisation off, the
program as written; hand and hand-inline = tags set in the source;
compiler); closed loop and at offered rates of 0.5 to 4 M records a
second. 14:50 to 15:26 local. Every build computed the same checksum.

Frequency not fixed: 3.57 to 4.12 GHz per run (median 4.11); outside-busy
median 0.000, at most 0.465. Control drift 2.8%, no trend.

## Throughput

Closed loop, glibc, M records a second: compiler 16.45, hand-inline 16.44,
hand 13.79, as written (generic) 8.70; adapters: rigtorp_inline 14.05,
moodycamel_rw_inline 11.36, moodycamel_rw_sp 9.96, moodycamel_cq_sp 5.50,
shim_generic 7.34.

The compiler's build of the unmodified source over as written: 1.89×
(glibc), 1.76× (jemalloc); over moodycamel_cq_sp 2.99× and 2.45×; equal to
hand-inline (1.00×), which it is byte for byte. The mechanism pass: 0
allocations a record against 2, 846 cycles a record against 2,396.

## Latency under load (p99, µs, glibc)

| offered rate (M/s) | 0.5 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| as written (generic) | 0.33 | 0.43 | 1.21 | 2.79 | 3.49 |
| compiler | 0.29 | 1.59 | 1.06 | 1.96 | 1.72 |
| shim spsc_inline (adapter) | 0.29 | 1.56 | 1.22 | 1.96 | 1.75 |
| rigtorp_inline | 0.33 | 0.31 | 0.31 | 0.33 | 0.51 |
| moodycamel_rw_inline | 0.30 | 0.31 | 0.30 | 0.34 | 0.43 |
| moodycamel_cq_sp | 0.72 | 0.72 | 0.77 | 0.94 | 242 |

No run was marked saturated.

## How far it holds

- A latency regression: at 1 M/s the compiler's build has 3.7× the p99 of
  as written, and every build on the runtime's ring shows it, while the
  expert rings stay at 0.3-0.5 µs at every rate. The likely cause is the
  consumer's slip in runtime/spsc_ring.hpp (pacing its polls by the
  producer's rate), added 23 October for throughput; it is being
  investigated. Until fixed, the ring is not level with the expert rings
  in tail latency.
- The 21 October timed leg measured 9.0× over the retired two-lock list;
  this leg supersedes it. The pipeline's gain is now bounded by its grain
  (eval-w3grain).

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
