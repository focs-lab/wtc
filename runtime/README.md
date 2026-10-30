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
| `stream_gate.hpp` | nothing: whether a single consumer waits for a batch, shared by the ring and the single-consumer form | none |
| `hazard.hpp` | nothing: hazard pointers for the segmented queues, one slot for pushes and one for pops | none |
| `waiter.hpp` | nothing: how a pop that waits for an element waits (spin, park, or spin then park), and the wake-up a publish owes a sleeping consumer, shared by every queue above | none |
| `bounded_ring.hpp` | a bounded ring a program wrote itself under a mutex (spdlog's `circular_q`), through `shim/bounded.hpp` | the ring's capacity and its answers to a full queue, read from its member functions (`docs/lifting.md`, "The bound"); with one producer or one consumer, that side is single |
| `bounded_ring.hpp`, through `shim/c_queue.cpp` | a ring of pointers a C program wrote itself under a pthread mutex, with an end count (PARSEC ferret's and dedup's `queue.c`) | the ring's capacity, read from its initialisation; with one producer or one consumer, that side is single; batches with any number of producers and consumers (`docs/lifting.md`, "C queues") |

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

`waiter` is what a waiting pop (`pop_wait`, `pop_wait_for` in the shim)
adds to a queue, the same for every implementation and without changing
any of them: it needs the queue's non-blocking pop and a call after every
publish. It is an eventcount. A consumer about to sleep counts itself
among the waiters, reads the futex word, looks at the queue once more, and
sleeps only if that look found nothing and only while the word is
unchanged; a publish reads the count of waiters and wakes one only if
someone is there. A sequentially consistent fence on each side, after the
consumer's count and after the producer's publish, is what keeps a
publish from slipping past a consumer that is going to sleep (the comment
at the top of the header gives the argument). The wait policy is chosen
when the queue is built: `spin` polls with a pause and then yields and
never sleeps, so a push owes it no wake-up and pays only a read of the
policy and a branch; `park` sleeps in
the kernel after one more look; `spin_then_park` polls for a budget of
`WTC_SPIN_NS` nanoseconds (4 µs, about what a sleep and its wake-up cost
together), converted to the processor's ticks at the rate
`stream_gate.hpp` measures, then sleeps. A queue whose policy can park
pays one fence and one load per publish, and the futex wake only when a
consumer sleeps. The fence makes the producer wait for the line its
publish wrote, which a polling consumer keeps taking, so on a queue that
streams it costs more than the rest of the push; the shim's default is
therefore `spin`. `close()` wakes every sleeper, and from then on a
waiting pop that finds the queue empty returns false; elements pushed
before it are still popped.

`bounded_ring<T>` is a bounded linearisable FIFO queue whose capacity is
exact. Pushes and pops take tickets from two counters, and ticket i is
cell i mod R, R a power of two at or above the capacity, each cell with a
sequence number as in Vyukov's bounded queue; but fullness and emptiness
are decided by the counters (tail - head against the capacity, the second
counter read after the first), not by the cells, so a pop stalled between
its ticket and its cell does not make the queue look full, and every
answer is the one a sequential bounded queue gives at the operation's
compare-and-swap. A push that overruns takes the oldest element's ticket
and its own in one compare-and-swap of head and tail packed into one
word. A ring built for batches keeps them in that word too: a run of
pushes or of pops (`try_push_n`, `consume_n`) decides its size k, the
least of what it asks for and the room or the elements there are, and
takes its k tickets in one compare-and-swap of the word, so a batch is
adjacent in the queue and sized as one appended or taken under a mutex
is, whatever the other threads on either side do. With two words a batch
would be sized by the other counter as read before its step, and a
batched pop and a batched push crossing there would each see the queue
before the other: two elements queued, capacity four, both asking for
ten, the pop takes two and the push appends two, where the mutex gives
two and four or four and two. The shim's
`wtc::bounded<T>` puts two waiters on it, one per side:
a pop waits while it is empty, a push while it is full, and each publish
wakes the other side's sleeper, with the same fences as the pop's
waiter; `close()` closes both.

A queue may be destroyed only after every push to it has returned (its
producers joined, or otherwise ordered before the destructor), not as
soon as its last element has been popped: a push wakes sleepers after its
element is visible, so a consumer can take the element while the push is
still reading and writing the waiter. A one-shot reply queue on the
requester's stack, destroyed when the reply arrives, would let the
replying thread write freed memory. Folly's and moodycamel's queues ask
the same.

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
two-consumer run of the segmented queue's single-consumer form and a
two-producer run of the spmc queue, which TSan has to report and
`WTC_CHECK` has to abort; and a waiter that drops its wake-ups, which the
waiting pop's test has to catch asleep. A suite that cannot fail proves
nothing, which is a lesson this project has already paid for once.

`bounded_test` runs the bounded queue through `wtc::bounded`: every
answer and every size against a sequential ring, on both layouts and
every topology; a pop held between its ticket and its cell while pushes
take the room it made (the exactness of the bound); producers that wait
for room and consumers that wait for elements at capacities one to three,
on every topology and policy; one producer kept against a full queue of
one, so that a pop lands in every phase of its going to sleep; `close()`
with producers asleep on a full queue and consumers on an empty one; and
discards and overruns under contention, where every push must have
arrived, been refused or been overrun, once. Its waiters with the wake-ups
dropped must leave a producer asleep against room, and a second producer
on a queue built for one must be caught by `WTC_CHECK`.

`c_queue_test` runs the runtime of lifted C queues through its C entry
points: every answer of the single and batched forms, the queue's contents
and the caller's ring indices against the original's ring with its end
count, at capacities zero to six; producers pushing single elements and
batches against one consumer popping batches, where every element must
arrive once, in order per producer, and every batch pushed as adjacent
elements; four producers against four consumers popping batches with
random limits and room, where every batch popped must be an adjacent run
of one order of all elements that keeps each producer's order and its
batches adjacent; a batched pop and a batched push crossed between
deciding their sizes and claiming their tickets (a hook compiled into
`c_queue_crossing` only), whose sizes must be an outcome of the
original's mutex; consumers asleep until the last producer terminates. More
terminations than producers must end the program when the queue finishes
at exactly that many (the program tests `==`), and change nothing when it
finishes at at least that many (`>=`); a limit beyond any `int` is no
limit; a batch with nothing to move, woken in place of a push or pop that
has something to move, must pass the wake-up on; and destroy, called by a
consumer as soon as it has seen the queue finished, must wait for the
last termination still inside the runtime (a hook compiled into
`c_queue_destroy` only). A second consumer on a queue built for one must
be caught by `WTC_CHECK`.

`waiter_test` runs the waiting pop through the shim's queue on every
topology and every wait policy: the lost wake-up's window forced (the
consumer held between its last look and its sleep while the producer
publishes, and while it closes the queue); one element at a time, pushed
after a random delay so that it lands in every phase of a consumer going
to sleep, and waited for before the next; producers pausing at random,
pushing one by one and in runs, with every element taken before the queue
is closed; `close()` with consumers asleep, with and without elements
left; and the time limit. A consumer left asleep with an element queued is
reported as a lost wake-up, with a deadline, rather than hanging the run.

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
