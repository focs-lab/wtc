# eval-w3grain: where the gain goes, 21 October 2026

`campaign.sh eval-w3grain` at 63c2ba7 on focs, three cores with their SMT
siblings offline (processors 4 to 6), governor performance, turbo off, 10
repetitions. The pipeline of eval-w3, closed loop, with each stage's own
work swept from 0 to 16,384 rounds a record. It ran 04:42 to 06:00 local
on 22 October (20:42 UTC on the 21st, which names the directory); the host
was busy 3.1% on average over the leg. Control: shim_generic 1.14 Mops/s
at the start, 1.10 at the end, drift 3.4%; no trend across repetitions.
Every grain's checksums agree across all ten builds.

This run replaces the one at 915a601, for the adapter pipeline's sake: its
sink recorded samples on a line the other stages read (see eval-w3; fixed
in 5e682b6). The W3wtc builds were not affected and not rebuilt, and the
table below repeats the earlier run's to within its noise.

## The gain against the grain

The compiler's pipeline against the program as written (W3wtc), with the
time a record takes through the slowest stage, measured as the inverse of
the build's throughput:

| rounds | compiler µs/record | generic µs/record | difference µs | speed-up |
|---|---|---|---|---|
| 0 | 0.105 | 0.962 | 0.86 | 9.2x |
| 4 | 0.112 | 0.963 | 0.85 | 8.6x |
| 16 | 0.58 | 1.70 | 1.12 | 2.9x |
| 64 | 2.46 | 3.61 | 1.15 | 1.47x |
| 256 | 10.1 | 11.2 | 1.12 | 1.11x |
| 1024 | 41.4 | 42.7 | 1.24 | 1.03x |
| 4096 | 178 | 179 | 1.43 | 1.01x |
| 16384 | 712 | 714 | 1.58 | 1.00x |

From 16 rounds on, the program as written pays an almost constant 1.1 to
1.6 µs a record more, which is what its two general-purpose queues cost
(four allocations and four frees a record in eval-w3's mechanism pass).
The speed-up is therefore close to (t + 1.1 µs) / t for a bottleneck stage
of t µs a record: above 10% only while a stage takes under about 10 µs,
and within the noise from about 100 µs. At zero work the stages cost less
than the queue, so the gain is the queue's whole cost, 9 times.

Hand-written tags trace the same curve: `hand-inline` equals the compiler
at every grain, and `hand` (by pointer) reaches 1.41x at 64 rounds against
the compiler's 1.47x.

In the adapter pipeline the inline queues (shim_spsc_inline,
rigtorp_inline, moodycamel_rw_inline) meet from 16 rounds on, at 1.70 to
1.73 Mrec/s. Below that the shim's ring leads: 9.1 against 5.6 and 6.0
Mrec/s at zero work, 8.4 against 7.4 and 6.9 at four rounds. At these two
grains the inline queues spread widely across repetitions (rigtorp's from
4.9 to 7.2 at zero work), as they did before the fix; the medians order
them, the single runs do not.

## What it says about the applications

PARSEC ferret spends milliseconds a record in its stages and dedup tens of
microseconds a chunk, both to the right of the knee, where the curve
predicts a few percent at most; the gains measured on them are within 1%.
The gain the compiler buys shows in programs whose stages take a
microsecond or less a record: logging, message passing, fine-grained
pipelines.
