# eval-w2: latency against offered load, 21 October 2026

`campaign.sh eval-w2` at 63c2ba7 on focs, two cores with their SMT
siblings offline (processors 4 and 5), governor performance, turbo off, 10
repetitions of eight queues at nine intervals, 64-byte elements, open loop.
It ran 03:10 to 03:47 local on 22 October (19:10 UTC on the 21st, which
names the directory); the host was busy 2.3% on average over the leg.
Control: shim_generic 1.11 Mops/s at the start, 1.08 at the end, drift
2.5%; no trend across repetitions.

This run replaces two earlier ones, at 915a601 and fae9abe. In both, the
consumer recorded its samples into vectors whose headers sat on the main
thread's stack beside the producer's per-element counters, so every record
wrote a cache line the other core was writing too, and every latency
included the benchmark's own false sharing. 5e682b6 keeps each thread's
bookkeeping in its own locals; in an untimed run on the container host it
moved the inline rings' median from 290-350 ns to 195-255 ns.

Latency runs from each element's due time to its receipt, so a queue that
falls behind shows its backlog instead of hiding it.

## The highest rate each queue keeps up with

A run keeps up when its median latency stays within a few microseconds;
above that, latency is a backlog that grows with the run.

| queue | class | keeps up at | falls behind at |
|---|---|---|---|
| shim_generic | B0 | 0.5 M/s (2,000 ns) | 1 M/s in 4 runs of 10 (median up to 27 ms) |
| onetbb_cq_sp | B1 | 1.4 M/s | 2 M/s (84 ms) |
| moodycamel_cq_sp | B1 | 1.4 M/s | 2 M/s in 7 of 10 (99 ms) |
| shim_spsc | compiler, by pointer | 2 M/s | 3.3 M/s in 7 of 10 (1.7 ms) |
| moodycamel_rw_inline | B2 | 5 M/s | 10 M/s in 6 of 10 (123 µs) |
| shim_spsc_inline | compiler, inline | 10 M/s, the end of the sweep | |
| ring_borrowed | runtime ring, no shim | 10 M/s | |
| rigtorp_inline | B2 | 10 M/s | |

The compiler's queue keeps up through the whole sweep, as do rigtorp's
queue and the bare ring, where the program as written falls behind at 1
M/s, the general-purpose queues at 2 M/s and moodycamel's inline SPSC
queue at 10 M/s. With the false sharing the compiler's ring stopped at 3.3
M/s and rigtorp's at 5; the benchmark's own traffic, not the queues, set
those limits. At 10 M/s the shim's ring and the bare ring are near their
limit, with a median of 0.9 to 1.0 µs against 0.33 µs at 5 M/s, and
rigtorp's is not (0.48 µs). The sweep stops at 10 M/s, so for the three
inline rings that keep up there these are lower bounds.

## Below saturation

At 10 µs between elements every queue keeps up, and what is left is the
cost of the handover itself:

| queue | p50 ns | p99 ns | p99.9 ns |
|---|---|---|---|
| shim_generic (B0) | 1,761 | 5,509 | 7,733 |
| onetbb_cq_sp (B1) | 971 | 2,175 | 6,179 |
| moodycamel_cq_sp (B1) | 898 | 2,060 | 6,147 |
| shim_spsc | 665 | 1,730 | 5,801 |
| moodycamel_rw_inline (B2) | 576 | 684 | 5,637 |
| rigtorp_inline (B2) | 474 | 565 | 5,462 |
| ring_borrowed | 393 | 506 | 5,327 |
| shim_spsc_inline | 372 | 542 | 5,323 |

The compiler's queue cuts the 99th percentile 10 times against B0 and 3.8
times against the best of B1, and is level with the expert queues; its
median is 100 ns below rigtorp's. The floor the inline queues share at
this frequency (1.9 GHz, turbo off) is 370 to 580 ns, against about 550 ns
with the false sharing.

The p99.9 column steps up by about 1.7 µs from the last run of the fourth
repetition on (the bare ring's, last in the order) and stays up for every
queue, while p50 and p99 stay where they were; before the step the inline
queues' p99.9 is 3.5 to 4.0 µs, as in the run at 915a601. A step that
every queue takes at the same moment is the host's, not a queue's: compare
p99.9 across the queues of this leg, not with other legs. The p99.99 and
the maximum (17 to 22 µs, 240 to 265 µs) are the same for every queue, as
before.

## What the mark does not see

`w2_latency` marks a run saturated when the producer or the consumer
sustained less than 95% of the offered rate (306932f). A backlog that does
not grow over the measured part, or that is worked off before its end,
leaves both rates within that and carries no mark: of the runs above with
a median over 10 µs, moodycamel_cq_sp at 500 ns has one marked of seven,
shim_spsc at 300 ns none of seven and moodycamel_rw_inline at 100 ns none
of six. The table above was read from the latencies. A mark from the
latency itself (median over some multiple of the interval) would catch
them; not yet done.
