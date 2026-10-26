# eval-w3grain: where the pipeline's gain ends, 27 October 2026

The 26 October leg (`2026-10-26-eval-w3grain`) run again on main at
e52c445 (the ring's gate, PR #37, among the changes since), with the W3
builds of `2026-10-27-eval-w3`. `campaign.sh eval-w3grain` with
`WTC_UNPINNED=1` on the two-socket AMD host, socket 1 (three cores), 5
repetitions under glibc and jemalloc; per-record work of each stage swept
from 0 to 16384 rounds, closed loop. Every build computed the same
checksum at every grain; the compiler's build equals hand-inline at every
grain (1.00×).

27 October 04:54 to 05:21 local (26 October 20:54 to 21:21 UTC). The
campaign names its directory by the UTC date, so on the second host this
leg wrote into `2026-10-26-*` beside the earlier leg; the directory here
carries the local date instead, and `meta.json` and the recorded commands
keep the UTC one. The leg appended to that directory's log and checksum
files; the copies here keep only this leg's lines. User clock median 4.11
GHz; outside-busy median 0.000, at most 0.010: a quiet leg. Control drift
3.5%, no trend.

## What it shows

The compiler's build over the program as written (generic), paired; the
26 October figure in brackets:

| work rounds | glibc | jemalloc |
|---|---|---|
| 0 | 1.90× [1.97] | 2.06× [1.98] |
| 4 | 1.93× [1.89] | 1.82× [1.78] |
| 16 | 1.19× [1.19] | 1.16× [1.17] |
| 64 | 1.04× [1.04] | 1.04× [1.04] |
| 256 | 1.01× | 1.01× |
| 1024 to 16384 | 1.00× | 1.00× |

The hand-over the specialisation removes is 0.039 µs a record at zero work
(1/12.69 − 1/25.15) and 0.055 at 4 rounds, as on 26 October: the gain is
above 10% only while a stage takes less than about 0.5 µs a record. The
gate changes the ring's latency under an offered load (eval-w2, eval-w3),
not its closed-loop throughput.

## How far it holds

- The pipeline's own model: one record per stage, closed loop. ferret (13
  ms an image) and dedup (57 µs a chunk) lie far to the right of the knee.

## Provenance

As `2026-10-27-eval-w3`.
