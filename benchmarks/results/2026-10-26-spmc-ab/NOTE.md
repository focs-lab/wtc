# spmc A/B: the lock-free queue against the ring whose consumers took turns, 26 October 2026

An untimed A/B on the container host (Intel Xeon Gold 6230, node 1,
processors 20 upwards, one per thread on distinct cores; governor
`powersave` with turbo, so frequency not fixed: 0.8 to 3.9 GHz per run,
median 3.0). Two builds of the benchmarks from the same tree but for the
runtime's queue for one producer and many consumers:

- `base`: `spmc_ring`, the spsc ring with its consumers taking turns under
  a spin lock (main at 056224a);
- `new`: `spmc_queue`, segments the producer fills alone, whose cells the
  consumers claim with a compare-and-swap (this branch).

The general queue (`segment_queue`, unchanged) is measured from the `base`
build. Ten repetitions; within each, configurations and variants in a fresh
random order, every variant of a configuration back to back, so that each
ratio is taken within a repetition. Other lanes built and tested on the
same node between and during repetitions (each repetition held the build
lock, but not every lane takes it), so single runs are noisy; the paired
ratios are what is reported. Median over the repetitions per
configuration, geometric mean of those medians across configurations, with
their range. `ab.py` recomputes every number here from the two run files.

## Why the ring lost

The 26 Oct pool leg (`2026-10-26-eval-pool`, another host) had the ring at
0.51× the general queue with eight workers and 1 µs tasks on the heap and
0.85× in the cell. Reproduced here at 0.43× (eight workers, 1 µs, 64-byte
capture, in the cell). An instrumented copy of W11, timing each `try_pop`
with the time-stamp counter (not committed), gave for that configuration:

| | pops that took a task | cycles each | empty pops |
|---|---|---|---|
| ring, turns under a spin lock | 1.49 M | 3,804 | 9,876 |
| general queue | 3.41 M | 305 | 732,028 |

Nearly every pop of the ring found a task, so it was not empty polls: each
pop waited in line for the lock, about seven others' turns. A turn moves
the lock's line, the ring's consumer state and the element's lines from the
previous consumer or the producer, so the pool ran at one turn per 370 ns,
2.7 M tasks a second, below the 8 M that eight workers of 1 µs could take.
The general queue makes only its fetch-and-add on the claim index serial.
The new queue does the same with a compare-and-swap. A first version of it,
which reloaded the head after a failed claim, took 308 cycles a pop in the
same configuration, 6.7 M tasks a second, with 0.08 failed claims per pop
at 1 µs of work and 1.4 at 100 ns.

## W11, the pool (36 configurations: 1, 2, 4, 8 workers; 0, 100, 1000 ns; 16, 64, 256 bytes)

| | task in the cell | task on the heap |
|---|---|---|
| new / general queue | 1.76× (1.02-10.7), none below 0.95 | 1.31× (1.03-7.2), none below 0.95 |
| ring / general queue | 1.25× (0.40-10.5), 8 below 0.95 | 1.18× (0.36-8.0), 5 below 0.95 |
| new / ring | 1.41× (0.76-2.6) | 1.11× (0.70-2.8) |

With eight workers the new queue is 2.0× the general queue in the cell
(1.02-3.45) and 1.44× on the heap (1.04-2.77), where the ring was 0.90× and
1.08×. The configurations the pool leg flagged:

| workers, work, capture | new / general, cell | ring / general, cell | new / general, heap | ring / general, heap |
|---|---|---|---|---|
| 8, 1000 ns, 16 B | 1.03 (0.95-1.11) | 0.40 (0.34-0.55) | 1.04 (0.96-1.06) | 0.36 (0.33-0.47) |
| 8, 1000 ns, 64 B | 1.02 (0.82-1.06) | 0.42 (0.36-0.61) | 1.18 (1.10-1.43) | 0.80 (0.51-1.02) |
| 8, 1000 ns, 256 B | 1.04 (1.01-1.10) | 0.48 (0.43-0.65) | 1.10 (1.02-2.05) | 1.59 (1.39-2.53) |
| 4, 100 ns, 64 B | 1.04 (0.87-2.50) | 0.55 (0.49-1.34) | 1.18 (1.12-1.26) | 0.98 (0.90-1.14) |

Where the new queue is behind the ring:

- One worker, no work: 0.87× at 64 bytes and 0.76× at 256 in the cell. The
  ring's single consumer pays one exchange per pop; the queue pays a
  compare-and-swap and reads the producer's count when it catches up. The
  compiler does not choose spmc for one consumer: a count it can settle
  gives spsc (or spsc_inline), so this is the case of a pool whose size is
  only known at run time and turns out to be one.
- Tasks on the heap with large captures and several workers: 0.70× at four
  workers, 100 ns, 256 bytes; 0.71-0.73× at eight workers and 256 bytes. The
  new queue is still above the general queue there (1.06-1.25×); the ring
  is above both. The likely cause, inferred and not isolated, is the
  allocator: the workers free captures the submitter allocated, and the
  ring's turns serialise those frees, which glibc's arena lock otherwise
  makes the workers and the submitter contend for.

## W9, split, one producer (the shim's tags; 8-byte elements, shared_ptr pops)

| consumers | new / general | ring / general | new / ring |
|---|---|---|---|
| 1 | 1.03 (0.94-1.40) | 1.15 (1.11-1.84) | 0.86 (0.76-1.01) |
| 2 | 1.00 (0.95-1.03) | 0.60 (0.56-0.88) | 1.67 (1.18-1.85) |
| 4 | 1.09 (0.91-3.10) | 0.47 (0.38-1.22) | 2.54 (2.02-2.97) |
| 8 | 1.13 (1.05-4.16) | 0.32 (0.27-1.02) | 3.66 (3.08-4.31) |

On this host the ring lost to the general queue from two consumers on;
the 26 Oct W9 leg on the AMD host had it ahead (2.2×). The new queue's
medians are level with the general queue or ahead at every count
(1.00-1.13); single repetitions dipped to 0.91.

## What it means for the compiler's choice

The substitution stays as it is: one producer and many consumers gets
spmc. The question the pool leg left, whether the ring needed a work
estimate before it could be the default, does not arise for the new queue,
which had no configuration's median below 0.95 against the general queue
(the lowest 1.00), over the 36 of the pool with either form of task and
the 4 of W9; single repetitions dipped to 0.71. The progress answers
change with it: the spmc pop is lock-free rather than blocking.

The program's code that a pop runs (the element's move or copy, the
shared_ptr's allocation) now runs after the claim, holding nothing another
consumer waits on. Under the ring it ran inside a turn: an element whose
move takes a lock that a popping consumer holds deadlocked under spmc and
not under the general queue (a liveness test written for that, run here
outside this branch's suite: the ring times out, the new queue passes,
plain and under ThreadSanitizer).

## How far it holds

- Untimed, on a shared node with frequency not fixed; the ratios are
  paired, but single repetitions vary widely, the general queue's most
  (its pop waits for a reserved cell, which a descheduled producer
  stretches).
- glibc only. The pool leg's allocator factor is not repeated.
- One host. The AMD host's pool and W9 legs should be rerun with the new
  queue before its numbers replace the 26 Oct ones there.
