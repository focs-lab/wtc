# eval-w9: general-purpose queues as threads are added, 21 October 2026

`campaign.sh eval-w9` at 63c2ba7 on focs, sixteen cores with their SMT
siblings offline (processors 4 to 19), governor performance, turbo off,
10 repetitions. Two shapes: split producers and consumers (P:C from 1:1 to
8:8, and one side single), and the pairwise workload of the lock-free
queue literature (each thread pushes and pops in turn, 1 to 16 threads).
With one consumer the shim's list for many producers (shim_mpsc) runs as
well, and with one producer its ring for many consumers (shim_spmc): what
the compiler substitutes for an mpsc or an spmc verdict. It ran 06:32 to
08:04 local on 22 October (22:32 UTC on the 21st, which names the
directory). Control: shim_generic 1.06 Mops/s at the start and at the
end, drift 0.6%; no trend across repetitions.

This run replaces the one at 915a601. There every consumer of the split
shape added to one shared atomic counter on every element and read it
before every pop, to know when the run was over: a contended line of the
benchmark's own, which the producers' flow control reads as well. 63c2ba7
has each consumer count locally and publish every 256 elements and when
the queue is empty. The queues that were fastest lost most to it: with one
consumer, moodycamel's queue rises 2.3 to 2.4 times from 4 producers on
and oneTBB's 1.5 to 1.7 times, the shim's list for many producers 1.2 to
1.5 times. The pairwise shape never had the counter, and its rows are
within 5% of the earlier run's.

## Where one side is single: what the compiler substitutes

Million elements a second, median of 10:

| P:C | shim_generic (B0) | compiler | moodycamel CQ | oneTBB |
|---|---|---|---|---|
| 2:1 | 1.26 | 3.10 (mpsc) | 3.60 | 3.45 |
| 4:1 | 1.05 | 3.39 (mpsc) | 5.38 | 3.89 |
| 8:1 | 1.03 | 4.10 (mpsc) | 5.49 | 4.37 |
| 1:2 | 1.09 | 3.18 (spmc) | 2.37 | 1.43 |
| 1:4 | 0.98 | 2.16 (spmc) | 2.24 | 1.49 |
| 1:8 | 0.86 | 1.71 (spmc) | 1.69 | 1.40 |

Against the general queue the substitutes gain 2.5 to 4.0 times (the list
for many producers) and 2.0 to 2.9 times (the ring for many consumers).
Against the libraries they differ. The ring for many consumers is ahead
of both at two consumers and level with moodycamel's from four on, where
its consumers' shared lock starts to tell. The list for many producers is
not on a par with the best libraries, as the earlier run made it look: it
reaches 87 to 94% of oneTBB's queue and 63 to 86% of moodycamel's, whose
producers each fill a sub-queue of their own and do not meet on one tail.

## Many on both sides: what the compiler leaves alone

| P:C | shim_generic (B0) | std::mutex queue | Boost lock-free | moodycamel CQ | oneTBB |
|---|---|---|---|---|---|
| 1:1 | 1.01 | 1.23 | 1.49 | 2.53 | 1.71 |
| 2:2 | 1.03 | 1.14 | 1.78 | 2.83 | 2.93 |
| 4:4 | 0.82 | 0.91 | 1.88 | 3.95 | 4.76 |
| 8:8 | 0.66 | 0.84 | 1.41 | 5.12 | 4.85 |

Pairwise, million operations a second: at 16 threads shim_generic 2.18,
std::mutex 2.20, Boost 3.15, oneTBB 4.18, moodycamel 9.81.

The general-purpose queue the compiler falls back to, a two-lock list,
does not scale: it loses throughput as threads are added and ends 7 to 8
times behind oneTBB and moodycamel at eight per side (5.5 to 6 times in
the earlier run, which held the libraries back). Every queue the analysis
cannot prove keeps it. This does not change what the compiler's
substitutions buy against the program as written, but the general
implementation is the weakest baseline in this group, and the list for
many producers is behind the best library too. A strong MPMC queue in the
general one's place, and a producer side that does not funnel every push
through one tail, are the shim's choices, not the analysis's, and the
cheapest improvements to reach for after the evaluation.
