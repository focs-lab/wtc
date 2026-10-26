# eval-w3: a three-stage pipeline with the ring's gate, 27 October 2026

The 26 October leg (`2026-10-26-eval-w3`) run again on main at e52c445,
which carries the gate on the ring consumer's slip (PR #37). That leg
found the compiler's pipeline 3.7× the p99 of the program as written at 1
M records a second; this one asks whether the gate removes it. Same leg
otherwise: `campaign.sh eval-w3` with `WTC_UNPINNED=1` on the two-socket
AMD host, socket 1 (three cores), 5 repetitions under glibc and jemalloc;
the adapter pipeline with every baseline class, and the compiler's
pipeline in its four builds (`build_wtc_flavours.sh` at e52c445: generic =
specialisation off, the program as written; hand and hand-inline = tags
set in the source; compiler), closed loop and at 0.5 to 4 M records a
second. The compiler's build and hand-inline are byte for byte identical
again (`flavours.sha256`: the two have the same sha256; the builds'
`flavours-build-info.txt` is beside it), and every build computed the same
checksum. The paired ratios leave out the mechanism pass (`"pass":
"mechanism"`), which is quoted separately below.

27 October 04:17 to 04:54 local (26 October 20:17 to 20:54 UTC). The
campaign names its directory by the UTC date, so on the second host this
leg wrote into `2026-10-26-*` beside the earlier leg; the directory here
carries the local date instead, and `meta.json` and the recorded commands
keep the UTC one. The leg appended to that directory's log and checksum
files; the copies here keep only this leg's lines. User clock median 4.10
GHz, 3.94 to 4.12; outside-busy median 0.000, at most 0.011: a quiet leg.
Control drift 0.7%, no trend.

## Throughput

Closed loop, glibc, M records a second: compiler 16.91, hand-inline 16.91,
hand 13.82, as written (generic) 8.90; adapters: rigtorp_inline 14.05,
the shim's inline ring 12.42, moodycamel_rw_inline 11.34,
moodycamel_rw_sp 9.91, shim_generic 7.34, moodycamel_cq_sp 5.46.

The compiler's build of the unmodified source over as written: 1.90×
(glibc), 1.81× (jemalloc) [26 October: 1.89×, 1.76×]; over
moodycamel_cq_sp 3.10× (glibc). The mechanism pass: 0 allocations a record
against 2.2, 922 cycles a record against 2,752.

## Latency under load (p99, µs, glibc)

| offered rate (M/s) | 0.5 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|---|
| as written (generic) | 0.33 | 0.40 | 1.47 | 2.22 | 2.80 |
| compiler | 0.29 | 0.28 | 0.27 | 0.29 | 0.29 |
| compiler, 26 October | 0.29 | 1.59 | 1.06 | 1.96 | 1.72 |
| shim spsc_inline (adapter) | 0.29 | 0.30 | 0.29 | 0.30 | 0.31 |
| rigtorp_inline | 0.31 | 0.31 | 0.31 | 0.31 | 0.31 |
| moodycamel_rw_inline | 0.30 | 0.30 | 0.31 | 0.31 | 0.32 |
| moodycamel_cq_sp | 0.72 | 0.72 | 0.73 | 0.88 | 1.22 |

Paired p99 of the compiler's build over as written, both allocators:
0.89× at 0.5 M/s, 0.78× at 1, 0.25× at 2, 0.15× at 3, 0.12× at 4 [26
October: 0.88, 4.21, 1.07, 0.90, 0.62].

- **The regression is gone.** The compiler's pipeline keeps a p99 of
  0.27-0.29 µs at every rate, level with the expert rings (0.30-0.32) and
  below the program as written everywhere, where on 26 October it was
  1.06-1.96 µs from 1 M/s.
- No run was marked saturated.

## How far it holds

- One host, frequency not fixed; this leg was quiet. The pipeline's gain
  is bounded by its grain (`2026-10-27-eval-w3grain`).

## Provenance

Tree synced from main at e52c445 (no git checkout on the host; `git_sha`
empty); the adapters rebuilt there (clang++-18, Release, native); the four
W3 builds made with tools/wtc-cc and wtc-opt at e52c445 on the first host
and copied over (their `build-info.txt` reads "dirty" only because of an
untracked link to the ClangIR toolchain in that tree).
