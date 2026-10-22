# eval-w8: batching a loop's critical sections, 21 October 2026

`campaign.sh eval-w8` at 915a601 on focs, eight cores with their SMT
siblings offline (processors 4 to 11), governor performance, turbo off, 10
repetitions. Each thread runs a loop of 2 million sections on one mutex
with a fixed amount of work between sections (0 to 500 ns, a dependent
chain the optimiser cannot fold). Builds: `plain` (one lock and unlock per
section), `batched` (the compiler holds the mutex across K sections) and
`hand` (the same batching written in the source), K = 64, plus K = 8 and
512 at two threads. It ran 16:15 to 16:41 local; the processors outside
the reservation were busy 2.9% on average. Control: shim_generic 1.14 and
1.12 Mops/s, drift 1.1%, no trend. Every build's checksums agree.

## Throughput

Million sections a second over all threads, K = 64:

| threads | work ns | plain | batched | batched / plain |
|---|---|---|---|---|
| 1 | 0 | 34.8 | 243 | 7.0x |
| 2 | 0 | 7.7 | 94.3 | 12x |
| 2 | 100 | 5.4 | 9.1 | 1.7x |
| 2 | 200 | 7.8 | 4.8 | 0.61x |
| 2 | 500 | 3.95 | 1.97 | 0.50x |
| 4 | 0 | 8.2 | 98.7 | 12x |
| 4 | 200 | 3.9 | 4.7 | 1.2x |
| 4 | 500 | 5.5 | 1.96 | 0.35x |
| 8 | 0 | 7.3 | 82.8 | 11x |
| 8 | 200 | 3.5 | 4.7 | 1.3x |
| 8 | 500 | 3.3 | 1.95 | 0.59x |

With nothing between sections, batching removes almost all the cost of
the lock: 11 to 12 times at two to eight threads. With work between
sections it does the opposite of what the plain loop does. The plain loop
runs that work in parallel, outside the lock; the batched loop runs it
under the lock, so its throughput is one thread's, whatever the thread
count (1.95 to 1.97 M a second at 500 ns, which is 1 / 500 ns). Batching
wins while the work is short against the cost of a contended acquisition
and loses once it is not: between 100 and 200 ns at two threads, between
200 and 500 ns at four and eight. The compiler's batching and the hand
batching agree within the intervals everywhere.

## Fairness

Jain's index over the threads' rates, median of the runs (1 is fair, 1/n
is one thread doing everything):

| threads | work ns | plain | batched |
|---|---|---|---|
| 2 | 200 | 1.000 | 0.938 |
| 2 | 500 | 1.000 | 0.903 |
| 4 | 100 | 1.000 | 0.750 |
| 8 | 100 | 1.000 | 0.587 |
| 8 | 500 | 1.000 | 0.587 |

The plain loop is fair at every point. A batched thread that holds the
mutex across K sections and their work starves the others, the more so
with more threads. At two threads K = 8 stays fair (0.96 or above) for a
smaller gain (42 against 94 M a second at no work), and K = 512 gains more
(215) but is unfair already at 20 ns (0.91).

## What it says about the transformation

Legal is not profitable. The compiler proves the batched loop equivalent,
but whether it pays depends on the work between sections, which it does
not yet estimate, and on how fair the program needs the lock to be. This
is R5 in the plan: an estimate of the work outside the section from the
CIR, a threshold from this leg (about 100 ns at K = 64), and K chosen by
the thread count rather than fixed.
