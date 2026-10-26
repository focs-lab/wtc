# eval-spdlog, rerun with the fixed waiting control, 26 October 2026

The same leg as `2026-10-26-eval-spdlog`, run again four hours later with
one build changed: orig-spin, spdlog as released with its queue waiting by
spinning, now spins outside the queue's mutex (31e944a) instead of
re-taking it after every pause. The earlier leg's control was slower than
the original for that reason alone, so its "queue alone" ratio was an upper
bound. This directory keeps the rerun; the earlier leg stays in
`2026-10-26-eval-spdlog`, whose `runs.jsonl` is byte for byte the copy
taken before this run overwrote the directory on the second host. The
`--json` paths inside `runs.jsonl` and `unpinned.tsv` name the directory the
leg wrote to, `2026-10-26-eval-spdlog`.

`campaign.sh eval-spdlog` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors, 5 repetitions under glibc,
jemalloc and mimalloc; 1, 2, 4 and 8 logging threads; payloads of 8, 64 and
256 bytes; one worker; null sink. 18:59 to 19:36 local (10:59 UTC). Builds
(benchmarks/apps/spdlog/build.sh): orig, generic, compiler and hand are the
binaries of the earlier leg, built from main at 5a70189; orig-spin was
rebuilt with `spin_original.py` at 31e944a.

Frequency not fixed: each run 1.02 to 4.09 GHz (median 3.67). Outside the
leg's set the processors were busy 0.051 of the time at the median and
0.169 at most; 409 of the 900 runs are above the 0.10 gate, spread evenly
over the five builds (72 to 87 each), so whole rounds were disturbed rather
than single builds: the leg was not quiet, and only paired ratios are
reported. Dropping the disturbed runs leaves the topology's share at 1.05
(on 34 of the 36 configurations, those with a quiet pair left) and orig-spin
/ orig at 0.96 (on all 36); ratios whose builds lose more configurations
move with the set: compiler / orig is 2.33 on the 33 left. The opening controls read
low (15.2 against 23.1 Mops/s at the close) while the repetitions are flat
(rep 0 to 4 within 0.7%): a warm-up that ended before the first
configuration, not drift.

## What the leg shows

Geometric means over the 36 (threads × payload × allocator)
configurations, with the range of per-configuration medians and the means
by logging threads (1 / 2 / 4 / 8):

| ratio | gm | range | by threads |
|---|---|---|---|
| compiler / orig | 2.52 | 1.21-6.35 | 1.41 / 1.61 / 3.38 / 5.33 |
| compiler / generic (the topology's share) | 1.05 | 1.01-1.13 | 1.08 / 1.06 / 1.03 / 1.03 |
| hand / compiler | 1.00 | 0.96-1.03 | |
| orig-spin / orig (the waiting alone) | 0.96 | 0.81-1.29 | 1.10 / 0.96 / 0.88 / 0.90 |
| generic / orig-spin (the queue alone) | 2.51 | 0.88-6.62 | 1.20 / 1.55 / 3.74 / 5.73 |
| compiler / orig-spin | 2.64 | 0.96-6.91 | 1.29 / 1.66 / 3.85 / 5.92 |

p99 of a call to `log()`, against spdlog as released (orig / compiler,
above 1 means the compiler's build has the shorter tail): 3.48 (0.67-17.4);
at 4 threads, 64 bytes, glibc, 11.7 µs against 1.6 µs. The compiler's
build has the longer tail in 3 of 36 configurations, all at 256 bytes under
glibc (2, 4 and 8 threads; at 4 threads 16.6 µs against 11.1).

- **The comparison with spdlog as released is now final for this host.**
  The compiler's build is faster in all 36 configurations (1.21× at least)
  and 2.52× on the geometric mean, as in the earlier leg (2.52×).
- **Waiting by spinning is not the gain.** With the mutex left alone the
  spinning control is level with the original's blocking wait (0.96×;
  ahead at one logging thread, 1.10×, behind from four, 0.88-0.90×). The
  gain over spdlog is the queue: the general-purpose queue the port gets
  without any proof is 2.51× the spinning control, and the compiler's
  choice of the single-consumer queue adds 1.05× on top.
- **The topology's share holds:** 1.05× (1.01-1.13), against 1.06×
  (1.01-1.17) in the earlier leg; the compiler's tag equals the hand tag.
- **Where the queue does not pay:** one logging thread with 256-byte
  messages, where the general queue is 0.88-0.93× the spinning control
  under all three allocators and the compiler's build 0.96-0.99× under
  glibc and mimalloc (level or ahead under jemalloc). The spinning control is not a build
  anyone ships; against the original it is still 1.2× or more.

## How far it holds

- Microbenchmark of the logging path: null sink, one worker. At 4 and 8
  logging threads the worker, not the queue, is the ceiling: every message
  carries a `std::promise` that allocates on both sides (found by the
  progress check on the worker, PR #30); this rerun does not change that.
- The binaries other than orig-spin predate the runtime changes of 26
  October (the single consumer's empty answer, `decisions/0003`), which
  touch the mpsc queue the compiler's build uses here.
- Frequency not fixed; the leg was not quiet (above); paired ratios only.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. Driven by campaign.sh at a19428f,
which writes `unpinned.tsv` with a header: run number, the first and last
`runs.jsonl` line the run wrote, effective user frequency in GHz, the
reference-cycle ratio (not read on this host), user cycles, task-clock, the
busy share of the processors outside the leg's
set, the allocator and the command.
