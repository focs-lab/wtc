# eval-spdlog: an asynchronous logger read as written, 26 October 2026

`campaign.sh eval-spdlog` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors, 5 repetitions under glibc,
jemalloc and mimalloc; 1, 2, 4 and 8 logging threads; payloads of 8, 64 and
256 bytes; one worker; null sink. Builds (benchmarks/apps/spdlog/build.sh,
main at 5a70189): spdlog 1.14.1 as released (orig), the same with its
queue waiting by spinning (orig-spin), the port with specialisation off
(generic), the compiler's build (compiler) and the queue tagged mpsc by
hand (hand). 14:12 to 14:50 local.

Frequency not fixed: each run 0.68 to 4.08 GHz (median 3.63); outside the
leg's set the processors were busy 0.372 of the time at the median, 0.623
at most: the noisiest of the 26 October legs. Paired ratios within a
repetition. Control drift 3.4%, no trend.

## What the leg shows

Geometric means over the 36 (threads × payload × allocator) configurations,
with the range and the means by logging threads (1 / 2 / 4 / 8):

| ratio | gm | range | by threads |
|---|---|---|---|
| compiler / orig | 2.52 | 1.21-6.09 | 1.42 / 1.60 / 3.37 / 5.25 |
| compiler / generic (the topology's share) | 1.06 | 1.01-1.17 | 1.09 / 1.07 / 1.03 / 1.04 |
| hand / compiler | 1.00 | 0.99-1.03 | |
| orig-spin / orig (the waiting alone) | 0.85 | 0.74-0.98 | 0.83 / 0.96 / 0.84 / 0.78 |
| generic / orig-spin (the queue alone) | 2.84 | 1.28-7.45 | |

p99 of a call to `log()`, 64-byte messages, glibc, 4 threads: 11.7 µs for
spdlog as released against 1.6 µs for the compiler's build.

Most of the gain over spdlog is the runtime's general-purpose queue, which
the program gets without any proof; the compiler's choice of the
single-consumer queue adds 6% on top, most at one logging thread. The
compiler's tag equals the hand tag.

## How far it holds

- The orig-spin control in this leg spins with spdlog's mutex re-taken after
  every pause, so its waiters contend for the lock the other side needs; it
  is slower than the original, not a clean "same queue, different waiting"
  control. The leg was run again the same day with a control that spins
  outside the mutex (31e944a): `2026-10-26-eval-spdlog-rerun`, where the
  queue's share is 2.51× and the other ratios are as here.
- The load from other lanes was high during this leg (outside-busy median
  0.37), which widens the absolute numbers; the paired ratios held within
  their ranges.
- The worker thread, not the queue, is the ceiling at 4 and 8 logging
  threads: every spdlog message carries a `std::promise` that allocates on
  both sides (found by the progress check on the worker, PR #30).

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
