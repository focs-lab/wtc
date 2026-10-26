# eval-w9: the general queue and its substitutes under many threads, 27 October 2026

The 26 October leg (`2026-10-26-eval-w9`) run again on main at e52c445,
with two runtime changes behind the substitutes: the queue for one
producer and many consumers is now lock-free (`runtime/spmc_queue.hpp`,
PR #48) instead of the ring whose consumers took turns, and the
single-consumer pop of the segmented queue (`shim_mpsc`) no longer answers
empty while a finished push is queued (decision 0003, PR #46). The general
queue and the libraries are unchanged. Same leg otherwise: `campaign.sh
eval-w9` with `WTC_UNPINNED=1` on the two-socket AMD host, socket 1, 5
repetitions under glibc, jemalloc and mimalloc, 8-byte elements, `split`
(P producers and C consumers on their own threads) and `pairwise` (1 to
16 threads that each push and pop). Every row allocates one object per
element, the shim on the consumer's side, the libraries on the
producer's; part of the shim's lead in `split` is likely that (inferred,
not measured), as the 26 October NOTE says.

27 October 02:23 to 03:35 local (26 October 18:23 to 19:35 UTC). The
campaign names its directory by the UTC date, so on the second host this
leg wrote into `2026-10-26-*` beside the earlier leg; the directory here
carries the local date instead, and `meta.json` and the recorded commands
keep the UTC one. The leg appended to that directory's log and checksum
files; the copies here keep only this leg's lines. Every run passed its
order and sum checks. User clock median 4.10 GHz; the slow runs are again
`std_mutex` and `orig_generic`, whose threads sleep. Outside the leg's set
the processors were busy 0.20 of the time at the median, 0.50 at most,
above the 0.10 gate in 1,637 of 2,213 runs: this leg was not quiet. On the
configurations where both runs of a pair were under the gate the ratios
below agree with all pairs on the same configurations (spmc over the
general queue 3.49 against 3.46; mpsc 1.45 against 1.38). Control drift
4.2%, no trend.

## What it shows

Paired ratios, median per configuration, geometric mean over shapes and
allocators with the range; the 26 October figure in brackets:

| comparison | shapes | ratio |
|---|---|---|
| shim_spmc / shim_generic | 1:1, 1:2, 1:4, 1:8 | 4.62 (1.94-14.3) [2.16] |
| shim_spmc / moodycamel CQ | same | 4.51 (2.08-10.1) [2.11] |
| shim_mpsc / shim_generic | 1:1, 2:1, 4:1, 8:1 | 1.31 (0.98-2.08) [1.29] |
| shim_mpsc / moodycamel CQ | same | 2.30 (1.82-3.57) [2.35] |
| shim_generic / moodycamel CQ | P = C, split | 1.67 (1.00-2.76) [1.67] |
| shim_generic / LCRQ | P = C, split | 1.59 (0.73-3.92) [1.61] |
| shim_generic / orig_generic | 2:2, 4:4, 8:8 | 7.90 (4.68-12.2) [7.88] |
| shim_generic / LCRQ | pairwise, 1-16 threads | 0.85 (0.68-1.07) [0.85] |
| shim_generic / LPRQ | pairwise | 0.89 (0.68-1.30) [0.89] |
| shim_generic / LSCQ | pairwise | 1.18 (0.84-1.69) [1.18] |
| shim_generic / fetch-and-add array | pairwise | 0.82 (0.70-1.03) [0.82] |

- **The spmc substitute now scales with its consumers.** Over the general
  queue it is 2.06× with one consumer, 3.15× with two, 4.99× with four and
  14.1× with eight (glibc medians: 28.7, 32.6, 45.4 and 29.1 M elements a
  second against 14.9, 10.6, 9.1 and 2.1); the turn-taking ring fell from
  28.0 to 4.9 over the same shapes.
- **The mpsc substitute is unchanged** by the new single-consumer pop:
  1.31× against 1.29×, most at 1:1 (1.93×), 1.12-1.21× at 2:1 to 8:1.
- The general queue and the libraries read as on 26 October. With both
  sides many it is ahead of moodycamel (1.29-2.45× by shape) and LCRQ
  (1.08-2.58×) in the geometric mean of every split shape, but not in
  every allocator at 8:8: under jemalloc it is level with moodycamel
  (1.00×) and behind LCRQ (0.73×). In the pairwise workload it trails
  LCRQ, LPRQ and the fetch-and-add array queue from 4 threads, 0.69-0.84×
  by thread count (0.68-0.88× per configuration).

## How far it holds

- Microbenchmark: 8-byte elements, nothing between operations; the leg
  was not quiet (above). One host, frequency not fixed.

## Provenance

Tree synced from main at e52c445 (no git checkout on the host; `git_sha`
empty), benchmarks rebuilt there (clang++-18, Release, native).
