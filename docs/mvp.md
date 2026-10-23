# What the compiler does now, and how to see it

Four things a library cannot do, because each needs to see the program and
not one call at a time. Every command below is run from the repository
root with the toolchain built as `docs/toolchain.md` describes; `wtc-cc`
compiles one C++ file to an executable through the whole pipeline and
prints, as remarks, what it decided and why.

```sh
tools/wtc-cc --toolchain build-llvm <options> <file.cpp> -o prog
```

## 1. A queue with one producer and one consumer

The gate's result: the compiler proves the topology and substitutes a
wait-free ring for the mutex-based queue. `docs/gate.md` has the
measurements.

```sh
tools/wtc-cc --toolchain build-llvm examples/queue_example.cpp -o prog
```

```
remark: wtc: queue @q1: spsc (producers: 1, consumers: 1)
remark: wtc: queue @q2: mpmc (producers: many, consumers: many)
remark: wtc: queue @q1: every pop only borrows its element (1 pop)
remark: wtc: specialised: constructor tag rewritten to spsc_inline
```

The queue may be a global, a local, a member of a struct or a heap object,
and the threads may reach it through parameters, struct fields or pointers
written in several places; a pointer that may name one of several queues
counts on each (`test/Transforms/QueueOwnershipAnalysis/`). `q2` has two
producers and two consumers and keeps the general-purpose queue; a side
that counts many says why -- two threads, as here, or one that stands for
many: started in a loop, under recursion, or from a function nothing in the
file calls (`why-many.cpp`). A queue with many producers and one consumer
gets the shim's linked list without locks, whose push is one exchange
(`test/Integration/mpsc_fan_in.cpp`); one with one producer and many
consumers, the SPSC ring with its consumers taking turns under a spin lock
(`spmc_fan_out.cpp`). `q1` goes one step further than the
gate did: its consumer only tests whether a pop returned anything, so it
borrows, and the next section is what that buys.

## 2. No allocation per element

The gate's second finding was that the queue's main cost is not its
synchronisation but the `shared_ptr` it allocates for every element: the
same ring with elements held inline was 10 to 41 times faster. Holding
them inline is only correct if the consumer never notices that the element
lives in the ring and dies at its next pop. The borrow analysis proves
that from the consumer's code, and the queue is then built with its
elements inline.

```sh
tools/wtc-cc --toolchain build-llvm examples/inline_pipeline.cpp -o prog
```

```
remark: wtc: queue @q: spsc (producers: 1, consumers: 1)
remark: wtc: queue @q: every pop only borrows its element (1 pop)
remark: wtc: specialised: constructor tag rewritten to spsc_inline
```

A consumer that keeps a copy (`last = p;`) is refused with that line
named, and its queue stays plain spsc. With `-DWTC_CHECK` a queue forced
inline over such a consumer aborts at the next pop instead of reading a
reused slot (`test/Integration/borrowed_retained.cpp`). On the pipeline
benchmark both queues are borrowed, and the compiler's build is byte for
byte the build whose source asks for inline storage itself.

## 3. Batching the pushes and the critical sections of a loop

A loop that pushes on every iteration pays one synchronisation per element.
In a function marked `WTC_BATCH` the compiler turns such a loop into one
batched run, published every `WTC_BATCH_K` elements (64 unless the program
sets it) and at the end, when nothing in the loop can be waiting for an
element it has not published.

```sh
tools/wtc-cc --toolchain build-llvm examples/batch_loop.cpp -o prog
```

```
remark: wtc: pushes in this loop batched: published every WTC_BATCH_K pushes and at the end
```

A loop that spins on an atomic, calls into the threading library, can
throw, returns from the middle or pushes only sometimes is left alone,
with the reason (`test/Transforms/Batch/refusals.cpp`).

The same mark lets the compiler hold a mutex across the critical sections
of a loop that takes and lets go of it on every iteration, letting it go
every `WTC_LOCK_BATCH_K` sections (64 unless the program sets it) and at
the end:

```sh
tools/wtc-cc --toolchain build-llvm examples/lock_loop.cpp -o prog
```

```
remark: wtc: critical sections in this loop batched: the mutex is let go every WTC_LOCK_BATCH_K sections and at the end
```

Every interleaving of the result is one the original allows, so no
critical section computes anything new; what changes is how long another
thread waits, and that the code between the sections now runs with the
mutex held. If that code waited for a thread that needs the mutex, the two
would wait for each other, so the rules are stricter than for pushes:
nothing in the loop may do input or output, sleep, or call anything the
compiler cannot see into other than the allocator and functions that only
touch the memory they are given (`test/Transforms/BatchLocks/refusals.cpp`).

## 4. Promises the build holds the program to

A queue the program declares single-producer and two threads push to:

```sh
tools/wtc-cc --toolchain build-llvm test/Transforms/DeclaredTopology/two-producers.cpp -o prog
```

```
error: wtc: queue @q is declared spsc but 2 threads push to it
note: push on a thread that starts in task1() at test/Transforms/DeclaredTopology/two-producers.cpp:25:22
note: push on a thread that starts in task2() at test/Transforms/DeclaredTopology/two-producers.cpp:26:22
```

A function declared lock-free that can block:

```sh
tools/wtc-cc --toolchain build-llvm -DLOG examples/progress_consumer.cpp -o prog
```

```
error: wtc: consume_one() is declared lock-free but can block
note: which does input or output (printf)
```

The same function passes as the compiler builds it without `-DLOG`: its
queue is spsc and borrowed, so the pop neither locks nor frees. It fails
with `--no-specialize` (the general-purpose pop takes a mutex) and with
`-DRETAIN` (keeping the element makes it a shared_ptr whose release may
free it).

The producer's side never passes, and `-DPRODUCER` says so: every push may
find its block full and allocate the next one, since the queue has no
capacity to run out of. Lock-freedom for a producer needs a bounded queue,
which the shim does not offer.

## How the substitutions are checked

A faster queue that races is not a result, so every number below stands on
`benchmarks/scripts/correctness.sh`, 27 checks, all passing on 20 October
2026:

- the ring's own tests, plain, under ThreadSanitizer and with its contract
  checks compiled in;
- the queue microbenchmark with every spsc and inline variant under
  ThreadSanitizer, at three element sizes, and the pipeline: no report;
- the compiler's own builds under ThreadSanitizer: the pipeline with both
  queues inline, and the batched producer loop over each implementation:
  no report;
- the contract checks silent on correct programs, the guard against a kept
  element included, on the compiler's inline builds;
- the contract check firing on a queue with two producers and two
  consumers forced to use the ring, in ten runs of ten.

The third item instruments the code the compiler generated, after the whole
pipeline (`tools/wtc-cc --tsan`); building the same source with another
compiler would check that compiler's code instead. A silent run proves
nothing unless the same path can report, so the test suite holds it to two
races it must find: a plain one on a global, which only instrumented loads
and stores can see, and the forced two-producer queue
(`test/Integration/tsan_own_build_caught.cpp`). The test suite also runs,
wherever a ThreadSanitizer runtime is found, the compiler's build of every
program a positive integration test runs; the one program with a single
thread is the exception.

## What it is worth

Measured on 19 October 2026 on the lab server under the gate's protocol:
three reserved cores with their SMT siblings offline, governor fixed,
turbo off, five interleaved repetitions of fresh processes, medians with
the range of the five. Results and summaries are under
`benchmarks/results/2026-10-19-*`.

**The pipeline, three stages, 64-byte records** (`w3_pipeline_wtc`),
million records a second, closed loop:

| build | median | range | vs general |
|---|---|---|---|
| general-purpose queue | 0.91 | 0.90–0.97 | 1.00× |
| spsc, tag set by hand | 3.18 | 2.99–3.54 | 3.48× |
| spsc_inline, tag set by hand | 8.40 | 8.21–8.66 | 9.20× |
| the compiler's choice: spsc_inline | 7.70 | 7.40–8.68 | 8.43× |

The compiler's build and the hand-inline build are the same bytes, so the
gap between their medians is the spread between runs, which their ranges
show. Paced at two million records a second, the general-purpose queue
cannot keep up (1.01 million a second); the others hold the rate, with a
p99 of 2.8 µs for spsc and 1.2 µs for both inline builds, and a p99.9 of
18.7 µs against 5.2 and 5.0 µs.

**Batching** (`w7_batch_wtc`), one producer loop, million elements a
second, closed loop, and the delay of every 64th element:

| implementation, 64 bytes | plain | batched | gain | delay p50 plain → batched |
|---|---|---|---|---|
| general-purpose | 0.99 | 1.91 | 1.93× | 2.9 → 34.1 µs |
| spsc | 3.25 | 3.38 | 1.04× | 0.9 → 19.0 µs |
| spsc_inline | 16.62 | 54.04 | 3.25× | 0.5 → 2.1 µs |

At 8 bytes the gain is 1.69× on the general-purpose queue and within 2% on
the two rings.

**Critical sections held as one** (`w8_lock_batch_wtc`), million sections
a second, closed loop, measured on 20 October 2026 (local time) in the
same way on four reserved cores:

| threads, work between sections | as written | batched | gain | finish spread, batched |
|---|---|---|---|---|
| one, none | 35.2 | 599 | 17.0× | — |
| one, about 15 ns | 33.4 | 58.8 | 1.76× | — |
| two, none | 7.08 | 201 | 28.4× | 0.6% |
| two, about 15 ns | 7.65 | 41.8 | 5.46× | 2.0% |

Two threads that take one mutex on every iteration hand its line back and
forth on every section; held for 64 sections at a time, it changes hands
64 times less often. The price is that the other thread waits longer for
it; here the threads still finished within 3.6% of the run of each other
(`benchmarks/results/2026-10-19-w8`). Batching pays where synchronisation is the cost: the mutex
queue, and the inline ring, where nothing else is left; on the spsc queue
the allocation per element still dominates. The delay is the price, and
the reason batching is the program's choice to make.

**Latency** (`w2_latency`, 64 bytes, one element every microsecond, open
loop): p99 of 11.7 µs for the general-purpose queue, 1.9 µs for the spsc
tag and 0.34 µs for spsc_inline, 0.03× the general-purpose queue's; at one
element every ten microseconds, 4.5, 1.6 and 0.33 µs. The tag's third arm
costs the general-purpose queue nothing: with two producers and two
consumers it runs at 0.93 million elements a second, as in the gate's
legs (`benchmarks/results/2026-10-19-w2mvp`, `-w5`).

**The queue alone** (`w1_throughput`, 64 bytes): the shim's spsc tag runs
at 2.64× the general-purpose queue (2.81 against 1.07 million a second),
against 3.07× in the gate's leg. The inline variants are bimodal in this
leg (the ring's slow regime, `runtime/spsc_ring.hpp`): the shim with
spsc_inline has a median of 7.9 and a range of 5.8 to 34 million a
second, and five repetitions do not settle a median between two regimes.
At 8 bytes, where the range is narrow, spsc_inline runs at 20.4× the
general-purpose queue and 6.8× the spsc tag.
