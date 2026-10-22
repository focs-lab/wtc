# eval-w7: batching a producer's pushes, 21 October 2026

`campaign.sh eval-w7` at 63c2ba7 on focs, two cores with their SMT
siblings offline (processors 4 and 5), governor performance, turbo off, 10
repetitions of the 48 builds `build_w7_flavours.sh` makes with
`W7_SWEEP=1`: three queues (generic, spsc by pointer, inline), three ways
of pushing (`plain`, `batch` by the compiler, `hand` with explicit batch
calls) and the batch size K in {1, 4, 16, 64 (the default), 256, 1024} at
64 bytes, plus the default K at 8 bytes. Closed loop, then paced at one
element a microsecond. It ran 06:01 to 06:31 local on 22 October (22:01
UTC on the 21st, which names the directory); the host was busy 2.1% on
average over the leg. Control: shim_generic 1.11 Mops/s at the start and
1.10 at the end, drift 0.4%; no trend across repetitions. Every build's
checksums agree.

The flavours' build line says "63c2ba7 (dirty)": the flavour scripts count
the untracked result directories as changes, which campaign.sh's own
check leaves out since 915a601. Nothing tracked was modified.

This run replaces the one at 915a601. There the consumer recorded its
samples into a global vector whose header shared a cache line with the
interval the producer reads on every element (fixed in 5e682b6). On a-p13
the fix moved W7 by 7% or less and left the compiler's remarks the same.
Here 43 of the 48 builds are within 3% of the earlier run; the five that
move more (3 to 10%, both ways) are generic and spsc rows whose
repetitions spread by more than 3% in both runs, and no conclusion below
changes.

## Throughput

Closed loop, 64 bytes, Mops/s:

| K | generic, compiler | generic, by hand | spsc, compiler | inline, compiler |
|---|---|---|---|---|
| none (`plain`) | 1.03 | | 3.48 | 25.0 |
| 1 | 1.09 | 1.07 | 3.39 | 25.0 |
| 4 | 1.64 | 1.65 | 3.70 | 25.0 |
| 16 | 1.84 | 1.83 | 3.61 | 25.0 |
| 64 | 1.90 | 1.99 | 3.49 | 25.0 |
| 256 | 1.83 | 1.95 | 3.28 | 25.0 |
| 1024 | 1.96 | 1.92 | 3.25 | 24.9 |

Batching pays only where a push takes a lock: the general-purpose queue
gains 1.8 times at the default K, most of it by K = 16. The spsc and
inline queues publish with one store an element already, and batching
neither helps nor hurts them beyond the noise. At 8 bytes the picture is
the same (generic 1.04 to 1.84, spsc 3.61 to 3.82, inline 26.2 in every
build).

The compiler's batching matches batching written by hand at every K and
on every queue, within the intervals (at K = 64 on the generic queue,
1.90 [1.88-2.09] against 1.99 [1.89-2.08]).

## What batching costs in latency

Paced at 1 M elements a second, median latency from the element's due
time, µs:

| K | generic | spsc | inline |
|---|---|---|---|
| none | 5.4 | 0.44 | 0.24 |
| 4 | 3.7 | 2.5 | 2.3 |
| 16 | 11.5 | 9.6 | 8.7 |
| 64 | 44.2 | 38.6 | 33.6 |
| 256 | 176 | 153 | 133 |
| 1024 | 688 | 605 | 532 |

An element waits in its batch until K elements have joined it: about K/2
intervals on average, K at worst (p99 1.0 ms at K = 1024). The batches
have no flush on idle or on a timer, so under light load batching turns
throughput the program does not need into latency it does. This is the
case the profitability item (R5 in the plan) has to cover alongside the
mutex result: batch where pushes contend, flush when the producer would
otherwise wait.

The generic queue without batching is at its limit at this rate (1.03
Mops/s closed): its runs spread from 2.6 to 12.7 µs, and one in ten falls
into a backlog of 12 ms, as one did in the earlier run (23 ms). Its
median, 5.4 µs here against 3.8 there, is that spread, not a change.
