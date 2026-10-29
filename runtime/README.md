# runtime

Specialised implementations the compiler substitutes for the general-purpose
ones in `shim/`. Each one is written and measured here *before* any pass
learns to emit it. That order matters: it establishes the ceiling. If a
hand-written single-producer, single-consumer ring is not meaningfully faster
than the general-purpose queue on a real workload, no amount of compiler work
will make it so, and it is better to find that out in week two.

Every implementation carries, in its header comment:

- the fact the compiler must prove before substituting it;
- what breaks if that fact is false;
- the measured numbers against the general-purpose baseline, and on what.

## What is here

| file | substitutes for | the fact to prove |
|---|---|---|
| `spsc_ring.hpp` | `wtc::queue<T>` | exactly one thread ever pushes, exactly one ever pops, and the destructor runs after both are done |
| `segment_queue.hpp` | nothing: it is `wtc::queue<T>`'s general-purpose implementation | none: any number of producers and consumers |
| `segment_queue.hpp`, single-consumer form | `wtc::queue<T>` | exactly one thread ever pops, any number push, and the destructor runs after all are done |
| `spmc_queue.hpp` | `wtc::queue<T>` | exactly one thread ever pushes, any number pop, and the destructor runs after all are done |
| `laned_mpsc.hpp` | `wtc::queue<T>`, only in a build with `WTC_LANED_MPSC`, as an experiment | the single-consumer form's fact, and that no thread can observe the order between elements of different producers |
| `stream_gate.hpp` | nothing: whether a single consumer waits for a batch, shared by the ring and the single-consumer form | none |
| `hazard.hpp` | nothing: hazard pointers for the segmented queues, one slot for pushes and one for pops | none |

`spsc_ring<Slot, kSlots>` is an unbounded wait-free ring of blocks of 128 KiB
each. It keeps the generic queue's semantics exactly: FIFO, `push` never
blocks or fails, `try_pop` never blocks and says empty only when the queue
was empty. The consumer keeps its distance from a streaming producer: when it
catches up and finds fewer than a batch of elements published it waits,
boundedly (about 64 pauses), for the batch, but only while
`stream_gate.hpp` reads the producer as streaming (an element faster than a
line crosses between cores; a paced producer is not waited for, apart from
a rare bounded probe that keeps the gate from locking shut), so that a slot line crosses the cores
once rather than once per element in it; the batch tracks the producer's
rate, one for a trickle and up to thirty-two lines' worth for a stream,
and the wait polls the producer's counter about as often as the batch
should take to fill, from an estimate of that rate, since every poll takes
the counter's line from the producer. The shim instantiates it, like every
other implementation, with `Slot = T`: the element lives in its slot, a
push copies or moves it there, and the consumer takes it by moving it out
(`try_pop(T&)`), by lending it in place (`spsc_inline`), or as the
`shared_ptr` the original API returns, which the consumer allocates from
the slot, so that no allocation ever crosses the threads.

`segment_queue<Slot, kManyConsumers>` is an unbounded linearisable queue over
a list of array segments ("FAAArrayQueue"): a push takes the tail segment's
next cell with one fetch-and-add and fills it, a pop takes the head
segment's next cell the same way, and a drained segment is freed through
the hazard pointers of `hazard.hpp`. A batch takes its cells with one
fetch-and-add. With many consumers a pop that reaches a cell no push has
reserved yet skips it after a short wait, and one that reaches a reserved
cell waits for it to be filled, so that a batch is never torn; the
single-consumer form never skips and takes no read-modify-write on its
side, and at a reserved cell takes the earliest full cell past it, so that
it never answers empty while a finished push's element is in the queue; it
does not wait, and costs O(1) amortised plus O(h²) in the h cells it has
passed and not taken (filled or not: a batch still being filled leaves up
to its length of them), allocating past 64 of them. A producer that stops for ever between
reserving and filling a cell keeps the consumer's head there, so the
segments after it are never freed.

`laned_mpsc<Slot>` gives up the one order the single-consumer form keeps
that a program may not need: between producers. Every producer thread
pushes into an spsc ring of its own, its lane, found through a table of the
thread's and made on its first push under the only lock; the consumer
takes up to 64 elements from a lane before it moves to the next, and
answers empty after one sweep of all of them finds nothing. Each
producer's order is kept exactly, so a batch still comes out whole; the
lane of a producer that has exited is drained and then freed. The queue
is not linearisable: a push that finished before another producer's push
began may come out after it. It exists to measure what that relaxation
is worth before any analysis tries to prove it safe; the shim carries it
only in a build that defines `WTC_LANED_MPSC`, under the tag
`mpsc_laned`, which the compiler does not know.

Contract checking: build with `-DWTC_CHECK` and every ring records the first
producer and the first consumer thread and aborts the moment a different
thread appears in either role. That turns each static verdict into a claim
the program checks at run time, and it is the negative control in the
benchmark protocol.

## Tests

`test/` is a standalone CMake project with no LLVM dependency. It builds with
the system compiler, which matters because the pinned ClangIR clang has no
sanitizer runtime.

```sh
cmake -S runtime/test -B build-rt -DCMAKE_CXX_COMPILER=clang++-18
cmake --build build-rt && ctest --test-dir build-rt --output-on-failure

# ThreadSanitizer variant; under g++-13 prefix ctest with `setarch x86_64 -R`
cmake -S runtime/test -B build-rt-tsan -DCMAKE_CXX_COMPILER=clang++-18 -DWTC_RT_SANITIZE=thread
# Contract-check variant
cmake -S runtime/test -B build-rt-check -DCMAKE_CXX_COMPILER=clang++-18 -DWTC_CHECK=ON
```

The suite includes tests that must fail: a two-producer run of the ring, a
two-consumer run of the segmented queue's single-consumer form, a
two-consumer run of the laned queue and a two-producer run of the spmc
queue, which TSan has to report and
`WTC_CHECK` has to abort. A suite that cannot fail proves nothing, which is
a lesson this project has already paid for once.

The lagging-consumer unit test found a real bug on its first run: the block
the producer starts in never received its `end` mark, read as drained one lap
later, and was reused under the consumer. The two-thread stress test had
passed, because in lockstep the consumer never fell a full lap behind. Keep
both kinds of test.

## Measured numbers

From `benchmarks/results/`, not from memory. Current, through the shim,
one producer and one consumer, 64-byte elements, geometric mean over five
allocators (`2026-10-28-eval-w1`, paired runs on a two-socket AMD host,
frequency not fixed):

| | general-purpose queue | this ring, `spsc` | this ring, `spsc_inline` |
|---|---|---|---|
| throughput | 15 M/s | 105 M/s, about 6.7× | 158 M/s, about 10.2× |

Over sizes and allocators the inline ring is 11× the general-purpose
queue (5.1–27×) and 0.84× rigtorp's ring (behind at 8 bytes, 0.87–1.30×
at 64 and 256). Latency (`2026-10-27-eval-w2`, after the gated wait):
at one element a microsecond the inline ring's p99 is 173 ns against 287
for the general-purpose queue and 180 for rigtorp's ring, and its median
the shortest measured at every rate.

The September gate's numbers (`docs/gate.md`), against the two-lock list
the general-purpose queue then was, are kept as history: 3.07× throughput,
a p99 of 1,716 ns against 10,906 at one microsecond offered, and the
observation that the inline ring ran 148 M/s against 3.6 with the
`shared_ptr` the shim handed back. That ordering of the two costs held for
the old queue; for the current one, whose element already lives in its
cell, removing the shared counters is the larger step (6.6× through the
shim, 8.4× in the ablation) and lending instead of allocating the smaller
(1.7×; see the README's "What each fact buys").
