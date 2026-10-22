# eval-w3: the three-stage pipeline, 21 October 2026

`campaign.sh eval-w3` at 63c2ba7 on focs, three cores with their SMT
siblings offline (processors 4 to 6), governor performance, turbo off, 10
repetitions, 64-byte records and four units of work per record, closed
loop and five offered rates. Two pipelines: the adapter pipeline with one
queue of every baseline class, and the compiler's pipeline (W3wtc) in four
builds of one source: `generic` (B0, the program as written), `hand` and
`hand-inline` (B3, tags written by hand) and `compiler`.

It ran 03:48 to 04:41 local on 22 October (19:48 UTC on the 21st, which
names the directory); the host was busy 3.0% on average over the leg.
Control: shim_generic 1.11 Mops/s at the start, 1.10 at the end, drift
0.8%; no trend across repetitions.

This run replaces the one at 915a601. There the adapter pipeline's sink
recorded its samples into a vector whose header sat on the main thread's
stack beside what the other stages read on every record, a false sharing
of the benchmark's own (fixed in 5e682b6). The W3wtc builds record nothing
on that path and were not rebuilt; their closed-loop rows repeat the
earlier run's within 3% (8.91 against 8.90, 3.04 against 3.09, 0.99
against 1.02).

## The compiler against hand-written tags

| build | Mrec/s, closed loop | allocations per record | cycles per record |
|---|---|---|---|
| compiler | 8.91 [8.90-8.96] | 0 | 785 |
| hand-inline (B3) | 8.92 [8.67-8.98] | | |
| hand (B3, by pointer) | 3.04 [3.01-3.17] | | |
| generic (B0) | 0.99 [0.92-1.06] | 4 | 7,677 |

The compiler's build runs as fast as the best tags a programmer could
write, 9.0 times the program as written; `hand-inline` and `compiler` are
byte for byte the same binary (the flavour script checks this), so the
remaining difference is noise. In the open loop at 4 M records a second,
only the inline builds keep up (median latency 590 ns, p99 3.1 µs); `hand`
falls behind there and in 4 runs of 10 at 3 M, `generic` at 1 M in 9 of
10.

## Against the libraries

The adapter pipeline puts each baseline's queue between the stages:

| queue | class | Mrec/s, closed loop |
|---|---|---|
| shim_spsc_inline | compiler's choice | 8.52 [8.46-8.56] |
| rigtorp_inline | B2 | 7.49 [7.03-7.78] |
| moodycamel_rw_inline | B2 | 7.02 [6.75-7.17] |
| shim_spsc | compiler's choice, by pointer | 2.98 [2.96-3.27] |
| moodycamel_rw_sp | B2, by pointer | 2.85 [2.79-2.90] |
| moodycamel_cq_sp | B1 | 1.77 [1.73-1.82] |
| onetbb_cq_sp | B1 | 1.60 [1.51-1.66] |
| shim_generic | B0 | 1.07 [1.03-1.12] |

Without the false sharing rigtorp's queue gains 19% (6.31 before); the
shim's inline ring is 3% lower (8.80 before) and every other queue moves
by 2% or less. The shim's inline ring stays ahead of both expert queues, by
14% and 21%, where in the bare handover of eval-w1 rigtorp's was ahead at
64 bytes. A likely reason, not yet checked with a profile: with work
between the stages the consumer seldom sits right behind the producer,
which is the regime that costs the ring at small elements. In the closed
loop a record's latency is the number in flight divided by the throughput,
and how many are in flight depends on which stage is the bottleneck, up to
the shared bound; the open-loop rows in SUMMARY.md are the latencies to
compare.

At 4 M records a second in the open loop, only the three inline queues
keep up, and the shim's has the lowest latency: median 641 ns and p99 3.0
µs, against 951 ns and 3.9 µs for rigtorp_inline and 1.0 µs and 4.4 µs
for moodycamel_rw_inline. Every queue by pointer and every general-purpose
queue falls behind.

The tails from p99 on are about 1 µs above the run at 915a601 for every
queue, the unchanged W3wtc builds included (the compiler's p99 at 4 M/s
was 2.1 µs and is 3.1 µs). eval-w2, the leg before this one, shows the same
step in its p99.9 from its fourth repetition on, for every queue at once:
it is the host's. Compare tails within this leg, not with other legs.
