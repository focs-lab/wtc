# eval-spdlog on the current runtime, 28 October 2026

The leg of `2026-10-26-eval-spdlog-rerun` repeated on main at abe26c6,
every build made again. Since that leg the queue the compiler gives
spdlog's pool of one worker, the single-consumer queue, takes the first
filled cell past one a push has taken and not yet filled instead of
answering empty (acac0fe), and its consumer waits for a batch only while
the producers stream (1d914d9); every queue now names the hazard slot
each operation uses (0208242). spdlog's port and the leg are unchanged.

`campaign.sh eval-spdlog` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors, 5 repetitions under
glibc, jemalloc and mimalloc; 1, 2, 4 and 8 logging threads; payloads of
8, 64 and 256 bytes; one worker; null sink. 16:41 to 17:18 local (08:41
UTC). The five builds (benchmarks/apps/spdlog/build.sh at abe26c6, made
in the evaluation's container and copied to the second host) and the
controls' CMake build (rebuilt there from the same tree) are all of this
tree.

Frequency not fixed: each run 0.95 to 4.09 GHz (median 3.63). The leg
was quiet, unlike the one it repeats: outside the leg's set the processors
were busy 0.001 of the time at the median and 0.068 at most, so no run is
above the 0.10 gate (409 of 900 were on 26 October). Control drift 1.9%,
no trend across repetitions; the paired ratios move by under 2% from one
repetition to the next (compiler / orig 2.45 to 2.49).

## What the leg shows

Geometric means over the 36 (threads × payload × allocator)
configurations of the per-configuration medians of the ratio within a
repetition, with their range and the means by logging threads (1 / 2 / 4
/ 8), beside the 26 October rerun's:

| ratio | 28 Oct | range | by threads | 26 Oct | range | by threads |
|---|---|---|---|---|---|---|
| compiler / orig | 2.46 | 1.16-6.55 | 1.31 / 1.53 / 3.39 / 5.38 | 2.52 | 1.21-6.35 | 1.41 / 1.61 / 3.38 / 5.33 |
| compiler / generic (the topology's share) | 1.02 | 0.91-1.12 | 0.98 / 1.01 / 1.03 / 1.04 | 1.05 | 1.01-1.13 | 1.08 / 1.06 / 1.03 / 1.03 |
| hand / compiler | 1.00 | 0.95-1.02 | | 1.00 | 0.96-1.03 | |
| orig-spin / orig (the waiting alone) | 0.96 | 0.81-1.29 | 1.10 / 0.96 / 0.87 / 0.91 | 0.96 | 0.81-1.29 | 1.10 / 0.96 / 0.88 / 0.90 |
| generic / orig-spin (the queue alone) | 2.53 | 0.93-6.94 | 1.21 / 1.58 / 3.76 / 5.70 | 2.51 | 0.88-6.62 | 1.20 / 1.55 / 3.74 / 5.73 |
| compiler / orig-spin | 2.58 | 0.93-7.35 | 1.20 / 1.60 / 3.88 / 5.96 | 2.64 | 0.96-6.91 | 1.29 / 1.66 / 3.85 / 5.92 |

p99 of a call to `log()`, against spdlog as released (orig / compiler,
above 1 means the compiler's build has the shorter tail): 3.74
(0.59-16.9); at 4 threads, 64 bytes, glibc, 11.5 µs against 1.6 µs, as on
26 October. The compiler's build has the longer tail in 3 of 36
configurations, all at 256 bytes: 2 and 4 threads under glibc (at 4,
17.4 µs against 11.1), as before, and 1 thread under mimalloc (3.8 µs
against 2.1; 1.6 on 26 October).

- **The comparison with spdlog as released holds**: the compiler's build
  is faster in all 36 configurations (1.16× at least), 2.46× on the
  geometric mean against 2.52×.
- **The gain is the general-purpose queue's**, as before: the queue the
  port gets without any proof is 2.53× the spinning control (2.51×), and
  spinning instead of blocking is level with the original's wait (0.96×).
- **The topology's share fell from 1.05× to 1.02×, all of it at one and
  two logging threads**: 0.98× and 1.01× there (1.08× and 1.06× on
  26 October), 1.03× and 1.04× at four and eight (1.03× both). Under
  glibc at one and two threads with 8- and 64-byte messages the
  single-consumer queue is now behind the general one (0.91-0.94×). Set
  beside the 26 October rerun (another day, so not paired; the same host,
  the frequency not fixed), spdlog as released, the spinning control and
  the general queue run at the rates they had (each 1.00× on the
  geometric mean of the per-configuration medians), while the compiler's
  build runs at 0.93× at one logging thread, 0.97× at two, and level at
  four and eight: the single-consumer queue got slower where its worker
  most often finds it empty. Both of its changes act there, the pop that
  walks past unfilled cells and reads the producers' count after four
  pauses on an empty cell (its own measurement put the idle empty poll at
  7.9 ns against 3.0), and the batch wait that now opens only while the
  producers stream; this leg does not separate them.
- The compiler's tag still equals the hand tag (1.00×).

## How far it holds

- Microbenchmark of the logging path: null sink, one worker. At 4 and 8
  logging threads the worker, not the queue, is the ceiling: every message
  carries a `std::promise` that allocates on both sides.
- Frequency not fixed; paired ratios only. The comparison with the
  26 October rerun across days is indicative.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of main at
abe26c6 on the second host, not a git checkout. The builds were made in
another copy of the same tree, not a git checkout either, so
`build-info.txt` left the commit and the ClangIR revision blank; they
were filled in by hand before the leg (abe26c6, 1ebf89d8c5e6), which is
what `meta.json`'s `build` shows. Driven by campaign.sh at abe26c6, which
writes `unpinned.tsv` with a header: run number, the first and last
`runs.jsonl` line the run wrote, effective user frequency in GHz, the
reference-cycle ratio (not read on this host), user cycles, task-clock,
the busy share of the processors outside the leg's set, the allocator
and the command.
