# eval-w3grain: where the pipeline's gain ends, 26 October 2026

`campaign.sh eval-w3grain` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors (three cores), 5
repetitions under glibc and jemalloc; the three-stage pipeline with the
per-record work of each stage swept from 0 to 16384 rounds, in every W3
build (main 5a70189, campaign.sh at a19428f). 15:45 to 16:13 local. Every
build computed the same checksum at every grain.

Frequency not fixed: 3.64 to 4.12 GHz per run (median 4.11); outside-busy
median 0.000, at most 0.004. Control drift 2.5%, no trend.

## What it shows

The compiler's build over the program as written (generic), closed loop;
the time per record is the compiler's build's (the slowest stage's work
plus the hand-over):

| work rounds | time per record | glibc | jemalloc |
|---|---|---|---|
| 0 | 0.039 µs | 1.97× | 1.98× |
| 4 | 0.061 µs | 1.89× | 1.78× |
| 16 | 0.273 µs | 1.19× | 1.17× |
| 64 | 1.13 µs | 1.04× | 1.04× |
| 256 | 4.59 µs | 1.01× | 1.01× |
| 1024 | 18.8 µs | 1.00× | 1.00× |
| 4096 | 80.6 µs | 1.00× | 1.00× |
| 16384 | 323 µs | 1.00× | 1.00× |

The hand-over the specialisation removes is about 0.04 µs a record
(1/12.96 − 1/25.49 at zero work, 0.05 at 4 rounds), so the gain is above
10% only while a stage takes less than about 0.5 µs a record. Against the
retired two-lock list (the 21 October leg) the constant was 1.1-1.6 µs and
the threshold about 10 µs: the stronger general-purpose queue the program
now gets without proof moved the knee down by a factor of twenty.

The compiler's build equals hand-inline at every grain.

## How far it holds

- The model is the pipeline's own: one record per stage, closed loop. A
  real program's stage time is what to compare with the 0.5 µs; ferret
  (13 ms an image) and dedup (57 µs a chunk) lie far to the right.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. The binaries were built from main at
5a70189 (the runtime, shim and workloads are unchanged from there to
3718285), driven by campaign.sh at a19428f, which writes `unpinned.tsv`
with a header: run number, the first and last `runs.jsonl` line the run
wrote ("-" for none), user GHz, the ratio to reference cycles (nan on this
host), user cycles, task-clock, outside-busy share, allocator, command.
