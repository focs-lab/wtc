# eval-w1smt: one producer, one consumer on SMT siblings, 21 October 2026

`campaign.sh eval-w1smt` at fae9abe on focs, in a bench reservation of one
core with its SMT sibling online (processors 4 and 60), so the producer and
the consumer share the core's L1 and L2. Governor performance, turbo off,
10 repetitions, 64-byte elements. It ran 22:29 to 22:33 local. The first
attempt, at 12:51, stopped at its opening control, which asked for two
distinct cores in a set of one; b3b76b1 runs the controls on the sibling
pair as well.

Control: shim_generic at 64 bytes, 3.10 Mops/s at the start and 3.06 at the
end, drift 1.3%; no trend across repetitions. Every row has a CoV under 1%,
against 1 to 13% on two cores.

| variant | siblings, Mops/s | two cores (eval-w1) | siblings / two cores |
|---|---|---|---|
| rigtorp_inline (B2) | 153 | 140 | 1.09 |
| shim_spsc_inline (compiler) | 67.0 | 66.8 | 1.00 |
| shim_spsc | 13.3 | 3.43 | 3.9 |
| moodycamel_rw_sp (B2) | 12.9 | 3.39 | 3.8 |
| onetbb_cq_sp (B1) | 9.93 | 1.85 | 5.4 |
| shim_generic (B0) | 3.07 | 1.11 | 2.8 |

Between siblings a cache line changes hands inside the core instead of
crossing to another one. The queues that pass every element by pointer
move several lines an element (the allocation the producer writes and the
consumer reads and frees, and for the general-purpose queues their nodes and
locks), and they run three to five times faster here. The inline rings move
a line for every few elements and gain nothing: their cost on two cores was
already the work an element takes, not the transfer.

So the compiler's gain over the program as written is smaller on siblings,
22x against 60x on two cores, and the order of the queues is the same. The
shim's inline ring runs at 0.44 of rigtorp's (0.48 on two cores): that gap
is the ring's own per-element cost, not a cost of crossing cores.
