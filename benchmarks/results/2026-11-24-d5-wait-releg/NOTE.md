# d5-wait-releg: the lifted wait after its oversubscribed branch changed, 24 November 2026

The second timed leg of wait lifting, measured at 14eb6f4. There the
oversubscribed branch of the lifted wait's policy (producers + waiters
above the processors of the affinity mask) parks after the 4 us pause.
Before, at 7c74233, measured in `2026-11-24-d5-wait`, it yielded for a
16 ms budget, and that leg found every budget below parking after the
pause. The binaries were built at the documentation commit on top of
14eb6f4, which changes no code. The leg also has one new arm,
`hand-park`: the hand patch's wait with `park`, which parks at once with
no pause. An untimed probe of unlifted queues (Q6) had plain park ahead
of the rule by 1.11 to 1.32 times.

`harness/leg_d5b.py` is `2026-11-24-d5-wait`'s harness with its cells
and variants changed and nothing else. Before the leg, an untimed A/B of
the two harnesses, two repetitions each on the same processors and the
same binaries for orig and hand-spin_then_park, gave a geometric mean of
0.971 (0.79 to 1.14 over 34 cells). The A/A pairs of the same runs
spread as far (0.94 to 1.14), so the change did not move what the two
arms measure.

The leg ran on focs, a bench reservation of 24 cores with their SMT
siblings offline (processors 4 to 27), governor performance, turbo off,
1.90 GHz on every run, 10 repetitions, variants rotated by repetition,
every run a fresh process under the host's exclusive measurement lock.
It ran 16:07 to 16:19 local (08:07 to 08:19 UTC), announced through the
host's leg driver. Its END line reads "busy mean 0.048; above 0.10 on
4-16,18-23", nothing outside the reservation. The per-run gate retired
none of the 600 runs (mean busy outside the set at most 0.021), and
every run checked its results. Controls: 0.257 and 0.269 M tasks/s,
+4.7%, under the 5% threshold. Drift: the last third of the repetitions
is 1.026 of the first. A/A: 0.96 to 1.02. orig, measured again with the
same binary, lands within 3% of the first leg in every oversubscribed
cell (0.272 against 0.274, 0.185 and 0.185, 0.177 and 0.177, 8.93 and
8.87 us, 0.241 and 0.249), so the two legs' figures can be set side by
side.

## Oversubscribed

Million tasks a second (ping-pong: round trip), median of 10, cores
busy:

| submitters, workers, processors | orig | hand, spin_then_park | hand, park | wtc now | wtc at 7c74233 (16 ms, first leg) |
|---|---|---|---|---|---|
| 1, 4, 4 | 0.274, 2.6 | 0.331, 3.4 | 0.380, 3.0 | 0.324, 3.3 | 0.317, 3.8 |
| 1, 8, 4 | 0.185, 2.8 | 0.285, 3.5 | 0.384, 3.0 | 0.312, 3.3 | 0.186, 4.0 |
| 1, 16, 4 | 0.177, 2.9 | 0.283, 3.5 | 0.388, 3.0 | 0.305, 3.3 | 0.152, 4.0 |
| 1, 8, 4, ping-pong | 8.87 us, 1.1 | 3.53 us, 3.4 | 8.57 us, 1.1 | 3.24 us, 3.3 | 7.34 us, 4.0 |
| 1, 16, 8 | 0.249, 3.3 | 0.309, 4.2 | 0.423, 3.4 | 0.358, 3.6 | 0.116, 8.0 |

The compiler's wait is now 1.18 to 1.72 times the original in throughput
and 2.7 times in round trip, and 0.98 to 1.16 times the hand patch with
the same policy. Against the 16 ms build it is level at 4 workers and
1.7 to 3.1 times faster from 8 workers on, the round trip 2.3 times
shorter, on 0.5 to 4.4 fewer cores.

## On 20 cores

| submitters, workers | orig | hand, spin_then_park | hand, park | wtc |
|---|---|---|---|---|
| 1, 2 | 0.475, 2.3 | 0.559, 2.8 | 0.592, 2.5 | 0.542, 2.8 |
| 1, 4 | 0.266, 2.8 | 0.354, 3.9 | 0.436, 3.3 | 0.373, 4.6 |
| 1, 8 | 0.242, 3.1 | 0.335, 4.5 | 0.429, 3.4 | 0.369, 3.6 |
| 1, 16 | 0.265, 3.1 | 0.315, 4.6 | 0.427, 3.4 | 0.365, 3.6 |
| 4, 8 | 0.307, 5.7 | 0.573, 11.1 | 0.611, 10.9 | 0.490, 10.7 |
| 4, 16 | 0.271, 6.7 | 0.381, 18.3 | 0.420, 17.7 | 0.358, 16.4 |
| 1, 1, ping-pong | 8.92 us, 1.1 | 5.48 us, 1.7 | 8.36 us, 1.1 | 5.23 us, 1.7 |
| 1, 4, ping-pong | 8.78 us, 1.1 | 3.23 us, 3.2 | 8.47 us, 1.1 | 5.37 us, 4.3 |

These cells did not change with the fix: wtc is 1.14 to 1.52 times the
original with one submitter (first leg 1.28 to 1.51) and 1.60 and 1.32
with four (first leg 1.66 and 1.30). It is level with the hand
spin_then_park at 2 and 4 workers and 1.10 to 1.16 times it at 8 and 16,
and 0.86 and 0.94 of it with four submitters, whose push keeps the
pool's mutex.

## The park arm

Parking at once wins every saturated cell: 1.25 to 2.19 times the
original, 1.06 to 1.37 times spin_then_park, 1.09 to 1.27 times the
compiler's wait (which reaches 0.79 to 0.92 of it), on fewer cores (3.0
to 3.4 against 3.3 to 4.6 with one submitter). That agrees with the
untimed probe's 1.11 to 1.32. With one task in flight it loses all of
it. Every round trip pays a futex wake, so park is 1.03 to 1.07 times
the original against 1.63 to 2.73 for the pause: 8.4 to 8.6 us round
trips against 3.2 to 5.5. A queue that is never empty rewards parking,
and a queue that empties after each element rewards the pause. Which of
the two a program is depends on its load, which the pass does not see.
The policy keeps the pause.
