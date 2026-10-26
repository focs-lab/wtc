# eval-w2: latency against offered load on the segmented queue, 26 October 2026

`campaign.sh eval-w2` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors (two cores per run), 5
repetitions of seven queues at nine intervals (100 ns to 10 µs, 10 to 0.1
M elements a second) under glibc, jemalloc and mimalloc; 64-byte elements,
open loop, spinning waits. 19:37 to 20:20 local (11:37 UTC). Binaries
built from main at 5a70189 (runtime, shim, adapters and W2 unchanged from
there to 73224b8, compared file by file).

This is the leg the 21 October one (`2026-10-21-eval-w2`, against the
two-lock list) said was to be run again on the current general-purpose
queue. It runs the runtime as it stood before three later changes: the
gate on the consumer's slip (PR #37), the single consumer's empty answer
(`decisions/0003`, 26 October) and the lock-free queue for one producer
and many consumers (PR #48). The first is the one this leg is about.

Latency runs from each element's due time to its receipt, so a queue that
falls behind shows its backlog. Frequency not fixed: each run 3.06 to 4.12
GHz (median 4.10). Outside the leg's set the processors were busy 0.001 of
the time at the median and 0.141 at most; 152 of the 945 runs are above
the 0.10 gate, 20 to 24 for every queue, so whole rounds were disturbed;
the paired ratios below move by at most 0.02 when those runs are dropped,
except three at 0.7 µs and faster: the p50 against as written, 5.10 with
them and 5.03 without; the p99 against as written, 1.23 and 1.11; the p99
against rigtorp's ring, 2.81 and 2.92. Control 16.0 Mops/s at the start,
15.3 at the end, drift 4.4%; no trend across repetitions.

## Which rates each queue keeps up with

Every queue keeps up through the whole sweep, 10 M/s, except moodycamel's
ConcurrentQueue (B1), which falls behind at 10 M/s (in 11 of 15 runs).
The program as written now keeps up where on 21 October, on the two-lock
list, it fell behind at 1 M/s.

## Median and tail by rate

glibc, medians of 5, ns; the other allocators give the same picture.

| interval | as written (B0) p50 / p99 | compiler, inline ring p50 / p99 | bare ring p50 / p99 | rigtorp (B2) p50 / p99 |
|---|---|---|---|---|
| 100 ns | 141 / 4,840 | 640 / 1,706 | 639 / 1,950 | 146 / 1,317 |
| 300 ns | 139 / 1,064 | 835 / 1,239 | 759 / 1,219 | 151 / 565 |
| 500 ns | 136 / 664 | 602 / 887 | 600 / 887 | 154 / 190 |
| 700 ns | 134 / 387 | 853 / 944 | 852 / 920 | 151 / 183 |
| 1 µs | 137 / 194 | 108 / 1,147 | 104 / 1,135 | 146 / 183 |
| 2 µs | 134 / 179 | 102 / 169 | 105 / 143 | 144 / 180 |
| 10 µs | 137 / 180 | 109 / 159 | 107 / 142 | 153 / 221 |

Paired ratios, compiler's inline ring over the other queue, geometric means
over the three allocators and the intervals named:

| against | p50, 0.1-0.7 µs | p99, 0.1-0.7 µs | p50, 2-10 µs | p99, 2-10 µs |
|---|---|---|---|---|
| as written (B0) | 5.10 | 1.23 | 0.81 | 0.69 |
| rigtorp's ring (B2) | 4.64 | 2.81 | 0.72 | 0.81 |

- **At 1.4 to 10 M elements a second the runtime's ring waits.** Its
  median is 600 to 850 ns where the program as written and the expert
  rings deliver in about 140; its p99 is 2.8 times rigtorp's. The bare
  ring (no shim) shows the same numbers (inline over bare 1.01× in p50,
  1.03× in p99), so the cause is in the ring, not in the shim; most likely
  it is the consumer's slip of 23 October, which waits for a batch of
  elements behind the producer that at these rates takes hundreds of
  nanoseconds to fill. This leg does not isolate it. This is the regression the pipeline leg
  (`2026-10-26-eval-w3`, 1.56-1.61 µs p99 at 1 M records a second) found;
  at 1 µs here the ring's median is back to 108 ns but its p99 is 1.15 µs
  against 0.19 as written.
- **At or below 0.5 M elements a second the compiler's queue has the shortest
  latency of the measured queues:** a median of 102-110 ns against 134-137
  as written and 144-153 for rigtorp's ring, a p99 of 159-169 ns against
  179-180 and 180-221.
- The as-written queue's p99 at 100 ns is long (4.8 µs under glibc, 0.16
  ms under jemalloc in a bimodal row) while its median stays at 140 ns;
  the cause is not isolated.

## How far it holds

- Microbenchmark: one producer, one consumer, nothing but the hand-over.
- A gate on the slip is proposed in PR #37 (open: wait for a batch only
  while the producer streams). Its untimed A/B on the container host covers
  W2 at 1 µs and 300 ns and W3 at 1-2 M/s; at one element a microsecond it
  took the share of elements 0.9-2 µs late from 46.7% to 0.7% (rigtorp's
  0.4%). A paired rerun on the second host is in progress; this leg is to
  be run again on that build before its numbers stand for the current
  runtime.
- Frequency not fixed, two cores of one socket, paired rounds.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. Driven by campaign.sh at a19428f,
which writes `unpinned.tsv` with a header: run number, the first and last
`runs.jsonl` line the run wrote, effective user frequency in GHz, the
reference-cycle ratio (not read on this host), user cycles, task-clock,
the busy share of the processors outside the leg's set, the allocator and
the command.
