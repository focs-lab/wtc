# eval-w2: latency against offered load with the ring's gate, 27 October 2026

The 26 October leg (`2026-10-26-eval-w2`) run again on main at e52c445,
which carries the gate on the consumer's slip (`runtime/stream_gate.hpp`,
PR #37): the ring's consumer waits for a batch only while the producer is
streaming, and probes for a stream with a back-off. That leg found the
ring waiting at 1.4 to 10 M elements a second; this one asks whether the
gate removes it. Same leg otherwise: `campaign.sh eval-w2` with
`WTC_UNPINNED=1` on the two-socket AMD host, socket 1 (two cores a run),
5 repetitions of seven queues at nine intervals (100 ns to 10 µs) under
glibc, jemalloc and mimalloc; 64-byte elements, open loop, spinning waits;
latency from each element's due time to its receipt.

27 October 03:35 to 04:17 local (26 October 19:35 to 20:17 UTC). The
campaign names its directory by the UTC date, so on the second host this
leg wrote into `2026-10-26-*` beside the earlier leg; the directory here
carries the local date instead, and `meta.json` and the recorded commands
keep the UTC one. The leg appended to that directory's log and checksum
files; the copies here keep only this leg's lines. User clock median 4.09
GHz, 3.30 to 4.12. Outside the leg's set the processors were busy 0.19 of
the time at the median, 0.41 at most, above the 0.10 gate in 778 of 953
runs: the leg was not quiet. Keeping only pairs whose runs were both under
the gate gives the same medians (p50 over as written 0.82 against 0.82 on
the same configurations) and a p99 over as written of 0.45 against 0.48.
The p99 against rigtorp's ring varies with the repetition (0.71 to 1.16
per repetition) and is the least firm number here. Control drift 2.2%, no
trend.

## Which rates each queue keeps up with

Every queue keeps up through 10 M/s except moodycamel's ConcurrentQueue,
which falls behind at 10 M/s (12 of 15 runs), as on 26 October.

## Median and tail by rate

glibc, medians of 5, ns:

| interval | as written (B0) p50 / p99 | compiler, inline ring p50 / p99 | bare ring p50 / p99 | rigtorp (B2) p50 / p99 |
|---|---|---|---|---|
| 100 ns | 143 / 6,109 | 109 / 2,073 | 106 / 2,055 | 146 / 1,838 |
| 300 ns | 135 / 1,260 | 106 / 648 | 106 / 696 | 146 / 617 |
| 500 ns | 133 / 800 | 106 / 200 | 106 / 165 | 151 / 230 |
| 700 ns | 132 / 562 | 109 / 170 | 106 / 148 | 147 / 179 |
| 1 µs | 133 / 287 | 106 / 173 | 106 / 156 | 146 / 180 |
| 2 µs | 133 / 169 | 106 / 149 | 107 / 139 | 149 / 169 |
| 10 µs | 136 / 175 | 112 / 149 | 109 / 146 | 156 / 239 |

Paired ratios, the compiler's inline ring over the other queue (below 1 is
shorter), geometric means over the allocators and the intervals named; the
26 October figure in brackets:

| against | p50, 0.1-0.7 µs | p99, 0.1-0.7 µs | p50 / p99 at 1 µs | p50, 2-10 µs | p99, 2-10 µs |
|---|---|---|---|---|---|
| as written (B0) | 0.81 [5.10] | 0.37 [1.23] | 0.82 / 0.84 [0.80 / 6.34] | 0.83 [0.81] | 0.70 [0.69] |
| rigtorp's ring (B2) | 0.74 [4.64] | 0.96 [2.81] | 0.72 / 0.93 [0.74 / 6.30] | 0.72 [0.72] | 0.83 [0.81] |

- **The regression is gone.** At every rate the ring (bare or through the
  shim) has the shortest median measured, 106-112 ns against 132-143 as
  written and 144-157 for rigtorp's ring over the nine intervals (the bare
  ring is the shorter of the two at 700 ns, 5 µs and 10 µs), where on 26
  October it waited 600-850 ns from 1.4 M/s. Its p99 is below as written
  at every rate (0.37× at 0.1-0.7 µs, 0.84× at 1 µs, 0.70× from 2 µs) and
  level with rigtorp's ring (0.96× at 0.1-0.7 µs, 0.93× at 1 µs, 0.83×
  from 2 µs).
- At 1 µs, where on 26 October the p99 was 1.15 µs against 0.19 as
  written, it is 0.17 µs against 0.29.
- The bare ring and the shim's inline ring agree (1.02× in p50), so the
  numbers are the ring's, not the shim's.

## How far it holds

- One host, frequency not fixed, a leg that was not quiet; the paired
  design and the checks above are what the ratios rest on, and the p99
  against rigtorp's ring is within its own spread of level.
- The as-written queue's p99 at 100 ns (6.1 µs) is long while its median
  stays at 143 ns, as on 26 October; the cause is not isolated.

## Provenance

Tree synced from main at e52c445 (no git checkout on the host; `git_sha`
empty), benchmarks rebuilt there (clang++-18, Release, native).
