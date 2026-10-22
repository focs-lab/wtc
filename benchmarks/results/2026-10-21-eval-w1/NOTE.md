# eval-w1: one producer, one consumer, 21 October 2026

`campaign.sh eval-w1` at 915a601 on focs, in a bench reservation of two
cores with their SMT siblings offline (processors 4 and 5), governor
performance, turbo off, 10 repetitions and a mechanism pass of 3. It ran
11:39 to 12:50 local, 72 minutes against an estimate of 40. Over the leg
the processors outside the reservation were busy 2.3% on average.

Control: shim_generic at 64 bytes, 1.04 Mops/s at the start and 1.08 at the
end, drift 3.7%; no trend across repetitions. Most rows carry a CoV above
3% (marked in SUMMARY.md); the intervals are the bootstrap 95% CI of the
median, and the conclusions below hold at either end of them.

## What the compiler buys

The compiler's output, shim_spsc_inline, against the program as written,
shim_generic (B0):

| element | shim_spsc_inline | B0 | speed-up | best expert SPSC (B2) | share of B2 |
|---|---|---|---|---|---|
| 256 B | 50.2 Mops/s | 1.52 | 33x | rigtorp_inline 50.0 | 1.00 |
| 64 B | 66.8 | 1.11 | 60x | ring_inline 148 | 0.45 |
| 8 B | 19.5 | 1.13 | 17x | rigtorp_inline 232 | 0.08 |

At equal interface (elements by pointer, `_sp`), shim_spsc runs level with
the expert queues: 3.4 Mops/s at 64 bytes against rigtorp_sp 3.7,
moodycamel_rw_sp 3.4 and boost_spsc_sp 2.6. The general-purpose queues
(B1) sit between B0 and the specialised ones: onetbb_cq_sp 1.85,
moodycamel_cq_sp 2.55, boost_ms_queue 1.83, std_mutex 1.31 at 64 bytes.
B0 is as slow as the original two-lock list (orig_generic 1.06).

The mechanism pass says where the factor comes from: B0 makes two
allocations and frees two per element and spends about 3,000 cycles on
it; the inline queue makes none and spends 34 to 155.

## What is left on the table

Two gaps, both in the runtime rather than in what the compiler proves:

- **The shim's layer.** shim_spsc_inline runs 122 instructions per
  element at 64 bytes where the ring under it, ring_borrowed, runs 53.
  The shim checks the tag in every operation and hands even a lent
  element back as a `shared_ptr` (with an empty control block), which the
  caller then destroys; how the 70 instructions split between the two is
  not yet profiled. At 256 bytes the copy hides them; at 64 bytes they
  halve the throughput.
- **The ring itself at small elements.** At 8 bytes ring_inline takes 136
  cycles per element at IPC 0.34, rigtorp_inline 22 at IPC 1.99, with
  about the same instruction count, so the difference is stalls. Both
  cache the opposite index. The ring's own comment
  (`runtime/spsc_ring.hpp`) describes a slow regime in which the consumer
  settles immediately behind the producer and every line the producer
  writes is still in the consumer's cache; with eight elements to a line
  that is the likely cause. rigtorp's queue is bounded, so a producer
  that runs ahead fills it and the two stay about a capacity apart, which
  would explain why it does not fall into the same regime; neither has
  been checked with a profile yet. ring_inline at 8 bytes ranges 27 to 39
  Mops/s across repetitions.

Neither changes the verdicts; both are work in `runtime/` and the shim:
keeping the consumer off the producer's line at small elements, and
resolving the tag once where the compiler already knows it.

## Not run

eval-w1smt, the same pair on two SMT siblings, failed in its first second:
its warm-up and control runs asked for two distinct cores in a reservation
of one core and its sibling. Fixed in campaign.sh afterwards and run again
as its own leg.
