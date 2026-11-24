# d5-wait: CTPL's wait lifted off its condition variable, 24 November 2026

The first timed leg of wait lifting (`wtc-cc --lift` replacing
`cv.wait(lock, pred)` by the eventcount's loop, PR #69, measured at
7c74233), and a sweep of the yield budget its policy uses when the
threads outnumber the processors. `harness/leg_d5.py` on focs, a bench
reservation of 24 cores with their SMT siblings offline (processors 4 to
27), governor performance, turbo off, 1.90 GHz on every run, 10
repetitions, variants rotated by repetition, every run a fresh process
under the host's exclusive measurement lock. It ran 13:26 to 13:44 local
(05:26 to 05:44 UTC) right after d4-delegate, announced through the
host's leg driver; its END line reads "busy mean 0.055; above 0.10 on
4-12,14,18-23", nothing outside the reservation. The per-run gate
retired none of the 940 runs (median 0.0037, highest 0.034); every run
checked its tasks' results (bad=0). Controls (orig, one submitter, 4
workers, three before and three after): 0.255 and 0.266 M tasks/s,
+4.3%, under the 5% threshold; no drift across repetitions (last third
1.000 of the first). The A/A arm is 0.98 to 1.01 of orig with one
submitter and 0.95 to 1.06 with four, which is how far apart two medians
of the same binary land in CTPL: most cells spread more than 3%, and
differences of 5% or less are not resolved.

## What ran

`bench` is `benchmarks/apps/ctpl/bench.cpp` (one submitter, the main
thread), `multi` probe Q5's version with four submitter threads; 0 ns
tasks, 1 s a run, 4096 tasks in flight, or 1 (ping-pong: the time per
task is a round trip). Main cells on 20 cores (4-23), oversubscribed
cells on 4 (4-7) or 8 (4-11).

| variant | build |
|---|---|
| orig | CTPL (437e135) as released, clang++-18 -O2 |
| hand-spin_then_park, hand-spin | probe Q5's hand patch (`ctpl_wait.py`: the worker's wait on the eventcount by hand), the policy fixed: pause 4 us then park, or pause then yield forever |
| wtc | the released source through `wtc-cc --lift`: the wait lifted, its policy chosen when the pool is built (one submitter: producers 1, waiters the pool's size; four: producers not known, so spin_then_park) |
| wtcy\<ns\> | wtc with the oversubscribed yield budget (`kLiftedOversubscribedYieldNs`, `shim/lifted.hpp`) set to \<ns\>; wtc is the 16 ms default, and its rebuild from an unmodified copy is byte for byte the same |

The policy wtc applies (`lifted_wait_policy`): producers + waiters above
the mask's processors: pause 4 us, yield for the budget since the wait
began, then park; else one producer and at most 4 waiters: the same with
64 us; else pause 4 us and park.

## Throughput on 20 cores

Million tasks a second, median of 10, and the ratio to orig:

| submitters, workers | orig | hand, spin_then_park | hand, spin | wtc | wtc / hand-stp |
|---|---|---|---|---|---|
| 1, 2 | 0.450 | 0.575 (1.28) | 0.577 (1.28) | 0.575 (1.28) | 1.00 |
| 1, 4 | 0.273 | 0.366 (1.34) | 0.386 (1.42) | 0.389 (1.43) | 1.06 |
| 1, 8 | 0.248 | 0.314 (1.26) | 0.209 (0.84) | 0.373 (1.51) | 1.19 |
| 1, 16 | 0.275 | 0.309 (1.12) | 0.127 (0.46) | 0.366 (1.33) | 1.18 |
| 4, 2 | 0.944 | 0.991 (1.05) | 1.008 (1.07) | 0.967 (1.02) | 0.98 |
| 4, 4 | 0.741 | 0.850 (1.15) | 0.861 (1.16) | 0.856 (1.16) | 1.01 |
| 4, 8 | 0.316 | 0.623 (1.97) | 0.558 (1.77) | 0.522 (1.66) | 0.84 |
| 4, 16 | 0.273 | 0.406 (1.49) | 0.405 (1.48) | 0.355 (1.30) | 0.87 |

Cores busy (CPU time over wall time, medians): with one submitter orig
2.3 to 3.1, wtc 2.9 to 4.6, hand-spin 2.9 at 2 workers rising to 16.3 at
16; with four submitters orig 4.6 to 7.0, wtc 4.6 to 16.9, hand 4.3 to
19.1.

With one submitter the compiler's wait is 1.28 to 1.51 times the
original, and where its rule picks spin_then_park (8 and 16 workers) it
is 1.18 times the hand patch with the same policy, on under half of
hand-spin's cores. With four submitters it trails the hand patch by 13
to 16% at 8 and 16 workers: the compiler keeps the pool's mutex around
the push, which the hand patch drops.

## Latency: one task in flight

| workers | orig | hand, spin_then_park | hand, spin | wtc |
|---|---|---|---|---|
| 1 | 9.0 us, 1.1 cores | 5.6 us, 1.7 | 5.4 us, 1.7 | 5.2 us, 1.7 |
| 4 | 8.9 us, 1.1 | 3.2 us, 3.3 | 6.4 us, 4.5 | 5.4 us, 4.3 |

Round trip per task, median. At 4 workers the rule spins (one producer,
at most four waiters) and is 1.7 times slower than parking after 4 us:
the spinners compete for the task and the queue's line.

## Oversubscribed: the yield budget

Ratio of medians to orig (the first column in M tasks/s; for ping-pong
the round trip):

| cell | orig | hand, spin_then_park | hand, spin | 64 us | 1 ms | 4 ms | 16 ms (wtc) | 64 ms |
|---|---|---|---|---|---|---|---|---|
| 1, 4 on 4 processors | 0.272 | 1.25 | 1.09 | **1.40** | 1.27 | 1.13 | 1.17 | 1.06 |
| 1, 8 on 4 | 0.185 | **1.53** | 1.02 | 1.21 | 1.14 | 1.10 | 1.01 | 1.00 |
| 1, 16 on 4 | 0.177 | **1.61** | 0.61 | 1.25 | 1.06 | 1.02 | 0.86 | 0.68 |
| 1, 8 on 4, ping-pong | 8.9 us | **3.6 us** | 11.8 us | 4.6 us | 5.9 us | 6.4 us | 7.3 us | 9.0 us |
| 1, 16 on 8 | 0.241 | **1.33** | 0.49 | 0.78 | 0.56 | 0.51 | 0.48 | 0.48 |
| 4, 8 on 4 | 0.345 | 1.65 | **1.92** | | | | 0.86 (policy spin_then_park) | |

Every spinning variant keeps the mask full (3.8 to 4.0 cores of 4, 6.6
to 8.0 of 8); spin_then_park uses 3.4 to 4.2.

The 16 ms budget does not hold. Throughput falls, within noise, as the
budget grows in every cell with one submitter, 64 us is the best budget
in all five, and 16 ms is 0.48 to 1.21 of the original and 0.36 to 0.93
of spin_then_park with no yield phase. With 8 processors every budget
loses to the original. Only at 4 workers on 4 processors does any budget
beat spin_then_park (64 us, 1.40 against 1.25). The budget was chosen
from one untimed cell on another host (a-p13), where 16 ms reached
spinning without end; here wtc with 16 ms is close to spinning without
end in three of the five cells (0.97 to 1.07 of hand-spin, 1.40 and 1.61
in the other two), but spinning itself is the loss: a waiter that yields
does not hand its processor to the thread with work often enough to pay
for the yields. The evidence of this leg is for the oversubscribed
branch to park after the pause, as the default does, rather than to
yield. The branch was changed so (14eb6f4), and
`2026-11-24-d5-wait-releg` measures the build that parks there.
