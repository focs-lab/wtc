# The gate: does the idea work?

The question this project had to answer before any more of it was built:
**can a compiler prove, on real C++, that a queue has one producer thread and
one consumer thread, substitute an implementation that exploits it, and leave
the program correct and measurably faster, without the programmer doing
anything?**

The answer is yes, and the measurements say by how much. They also say
something that changes what to build next, which is the more valuable half of
the result.

All figures below are medians of five repetitions, each a fresh process, on
reserved cores with their simultaneous-multithreading siblings offlined, the
governor fixed at performance and turbo off, one exclusive measurement lock
per run, variants interleaved so drift would hit them equally. The raw runs
and the generated summaries are under `benchmarks/results/`. Nothing here is
quoted from a run made outside a reservation.

## The short answer

| | |
|---|---|
| throughput, 64-byte elements | **3.07×** the general-purpose queue |
| tail latency at one microsecond offered, p99 | **0.16×** |
| tail latency at one microsecond offered, p99.9 | **0.08×** |
| on a three-stage pipeline, end to end | **2.8×**, and p99 391 times lower |
| the compiler's binary against a human's | **byte for byte identical** |
| where the proof fails | the general-purpose queue, at no cost |

Re-measured on 19 October 2026 (`benchmarks/results/2026-10-19-w1mvp`),
the first row came out at 2.64×: 2.81 against 1.07 million elements a
second, medians of five. The summaries print "smt on", and so did those of
the September legs: that is the machine-wide switch, while the reservation
takes each reserved core's sibling offline; the environment check now
verifies the siblings and records the answer in `meta.json`.

## The criteria, and what happened

### Correctness comes first

A faster queue that races is not a result. Four things were required, and the
fourth is the one that makes the other three mean anything.

Fifteen checks, all passing, run by `benchmarks/scripts/correctness.sh`:

- the ring's own tests, plain, under ThreadSanitizer and with the contract
  check compiled in;
- ten workload runs with the specialised queue under ThreadSanitizer, across
  three element sizes and both the microbenchmark and the pipeline: no
  report;
- the contract check silent on ten runs of a program where the proof holds;
- the contract check firing on ten runs of a program where it does not: a
  queue with two producers and two consumers, forced to use the
  single-producer ring.

That last one exists because a check that never fires looks exactly like a
correct program. This project has already paid for that lesson once, in a
test suite that could not fail.

ThreadSanitizer runs use the system Clang, because the pinned ClangIR
compiler was built without a sanitizer runtime. The script finds a compiler
whose sanitizer works by trying it, rather than trusting the environment's
`CXX`, which on this host names exactly the compiler that cannot.

### The prize is real

Throughput, one producer and one consumer, against the same shim with the
general-purpose implementation:

| element | specialised | general | ratio |
|---|---|---|---|
| 8 bytes | 3.51 M/s | 1.16 M/s | 3.03× |
| 64 bytes | 3.31 M/s | 1.08 M/s | 3.07× |
| 256 bytes | 2.72 M/s | 1.56 M/s | 1.75× |

Tail latency, open loop, 64-byte elements, the producer sending on a fixed
schedule:

| offered | p99 | p99.9 |
|---|---|---|
| one element per microsecond | 1,716 ns against 10,906 ns, 0.16× | 3,485 ns against 41,808 ns, 0.08× |
| one per ten microseconds | 1,650 ns against 3,945 ns, 0.42× | 1,717 ns against 6,049 ns, 0.28× |

The slide this project started from claimed two to three times. On
throughput that is met at the sizes that matter and missed at 256 bytes,
where copying the element costs more than the queue does. On the tail, which
is what the motivation actually rests on, it is beaten by a wide margin.

### The substitution is automatic

A three-stage pipeline with two queues, built three ways from one unmodified
source: the tag left alone, the tag set by hand, and the tag set by the
compiler's own verdict.

| build | queues report | throughput | p50 |
|---|---|---|---|
| untouched | generic, generic | 1.00 M/s | a growing backlog |
| tagged by hand | spsc, spsc | 2.93 M/s | 2,992 ns |
| tagged by the compiler | spsc, spsc | 3.07 M/s | 2,847 ns |

The checksum is identical in all three, so the three programs compute the
same thing. The compiler's binary and the hand-tagged binary are **byte for
byte identical**, and both differ from the untouched one in 3,298 bytes. No
measurement can separate the compiler's choice from the human's because
there is nothing to separate; the 5% between their timings is noise.

At the offered rate of two million records a second the untouched build does
not merely run slower. It cannot keep up at all, so its queue grows without
bound and its percentiles measure the length of a backlog rather than the
cost of an operation. Both specialised builds hold the rate exactly.

### Where nothing is proved, nothing is risked

On two producers and two consumers, where the analysis must refuse, the shim
carrying the tag runs at 1.06× the original file it replaced, which is to
say the branch costs nothing measurable and is if anything on the right side
of the noise. The compiler leaves that queue alone: on the original example
program it proves one queue single-producer and refuses the other, and
rewrites exactly one constructor.

## What the numbers say that the criteria did not ask

**The allocation, not the synchronisation, is what the queue costs.** The
same ring, with the element stored inline instead of behind the shared
pointer the shim's interface hands back:

| element | inline | behind a shared pointer | ratio |
|---|---|---|---|
| 8 bytes | 36.9 M/s | 3.6 M/s | 10× |
| 64 bytes | 148.0 M/s | 3.6 M/s | 41× |
| 256 bytes | 47.7 M/s | 3.1 M/s | 16× |

Every queue in the comparison that stores a shared pointer, ours or a
library's, lands between 2.5 and 4.2 million elements a second. Every one
that does not lands above ten million. The lock-free algorithm the compiler
substituted was never the dominant cost under this interface; it was the
smaller of two, and the larger one is a call to the allocator on every
element, plus a free on the other thread that goes through its own arena
lock.

This is the most useful thing the measurements produced, because it says
what to build next and it is squarely a compiler's job. Eliding the
per-element allocation needs a fact about the program, not about the queue:
that the consumer does not keep the pointer past the pop. That is an escape
analysis on a value whose shape is still visible in the intermediate
representation, and it needs the same whole-program view we already compute.
A library cannot do it, because a library cannot see the consumer.

**The ring is a competent one.** This had to be checked before any ratio
above meant anything: with elements inline it runs at 1.06× Rigtorp's flat
bounded ring, and behind the shared pointer at 1.05× moodycamel's
ReaderWriterQueue with the same slot type. The gains reported here are the
substitution's, not an artefact of comparing against something badly
written.

## The legs that inform rather than decide

None of these gates anything. They say what to look at next.

**The run-time contract check is free where it ships, and only there.**
Both legs measured in one session:

| build | without the check | with it | ratio |
|---|---|---|---|
| the shim, tagged spsc | 3.28 M/s | 3.29 M/s | 1.00× |
| the ring alone, shared-pointer slots | 3.47 M/s | 3.23 M/s | 0.93× |
| the ring, elements inline | 150.5 M/s | 67.3 M/s | 0.45× |

On the shim, which is what the compiler produces, the check is free to
three digits. On the bare ring it costs about 7%, and on the inline ring
more than half, because there an operation is a few nanoseconds and one
relaxed load and compare per operation is a large part of it. The check is
affordable in exactly the configuration the compiler emits and expensive in
the one it does not, so leaving it compiled in by default is worth
considering.

That leg carries a caveat of its own: its repetitions spread 22%, almost
all of it from the inline ring, whose own slow regime swings a single row by
a factor of eight. The 1.00 and the 0.45 are unambiguous; the 0.93 should
not be read closer than "a few per cent".

**Core placement does not matter here, but sharing a core does.** Putting
the two threads at opposite ends of the reservation rather than adjacent
changes nothing, 0.94 to 1.05 times across every variant, which is what one
die with one memory node should give. Putting them on the two hardware
threads of a single core is a different matter:

| variant | two cores | one core, both threads |
|---|---|---|
| general-purpose shim | 1.08 M/s | 3.07 M/s |
| specialised shim | 3.31 M/s | 12.66 M/s |
| ring, shared-pointer slots | 3.59 M/s | 12.71 M/s |
| ring, elements inline | 148.0 M/s | 141.2 M/s |

Every queue whose cost is a line handed between the threads, or an
allocation freed by the other one, runs three to four times faster when the
two threads share a core's caches. The inline ring, which is limited by
something else, does not change. The caveat is that these threads do almost
nothing but queue operations; threads with real work of their own would be
competing for one core's execution resources instead. It is still a
placement decision, of exactly the kind a compiler that knows the whole
program could make and a library cannot.

**The ring's block size, corrected.** An earlier note in the header claimed
the slow regime disappeared above 4096 slots. That was three repetitions
talking. With five, and one process per configuration rather than nine
configurations sharing one:

| slots | median | range |
|---|---|---|
| 64 | 8.2 M/s | 5.9 to 9.2 |
| 256 | 52.7 M/s | 5.1 to 72.6 |
| 1024 | 52.0 M/s | 7.0 to 61.5 |
| 4096 | 60.5 M/s | 7.3 to 65.7 |
| 16384 | 57.1 M/s | 7.8 to 61.9 |

Sixty-four slots is genuinely too small. Above that the median is flat to
about 15% and 4096 is at the top of it, which is why it is the default. But
the slow regime never goes away: at every size from 256 up, about one run in
twenty still lands in it. Padding the shared state to 128 bytes instead of
64 was tried and made no difference. Under the shim's shared-pointer slots
the whole question is moot, 3.2 to 3.4 million a second whatever the block
size, because the allocator decides.

The first version of this sweep ran all nine configurations inside one
process and produced numbers that said nothing: every row spanned a factor
of seven. Nine measurements sharing one heap, one set of pages and one warm
cache are not nine samples.

**The analysis costs less than the compile that feeds it.** On the example
with 7,605 lines of intermediate representation, lifting, analysing and
specialising take 0.60 seconds against 0.46 for producing that
representation in the first place. The bound that mattered was that it not
blow up, and it does not.

**oneTBB, the library the vision document names.** With one producer and one
consumer it runs at 1.71 times the general-purpose shim, behind moodycamel's
ConcurrentQueue at 2.33 and well behind the specialised queue at 3.26. With
two producers and two consumers, the shape it is built for and the shape
where the compiler correctly refuses to substitute anything, it is the
fastest general-purpose queue measured, 3.11 times the shim against
moodycamel's 2.33. A general-purpose queue cannot use a fact nobody told it,
and that is the whole argument in one row.

**A ping-pong cross-check.** Measuring half a round trip needs no agreement
between two cores about the clock, unlike the open-loop latencies. It puts
the specialised queue at 340 ns against the general-purpose 1,427, agreeing
in direction and magnitude with the open-loop figures.

## What is not proved

**One translation unit.** The analysis sees one file. A second file touching
the same global queue is invisible to it, and nothing enforces the
assumption.

**Queues reached through a member or the heap are refused.** Tracing stops at
a load that is not from a stack slot written once. That is the safe
direction, and it is also why the benchmark adapters, which hold their queue
as a struct member, cannot be the workload that demonstrates the compiler's
decision. A large class of real programs is in that refused set today.

**One machine, one shape.** A single socket, one memory node, two or three
cores per measurement, a pipeline and a microbenchmark. Nothing here says
anything about many queues, many cores, or another processor.

**The dialect still expresses nothing the input did not.** The topology
attribute is the first fact in it that ClangIR did not already carry. The
static checks the project describes need the rest, and none of them exists.

## What to do next, in order

1. **Elide the per-element allocation** when the consumer provably does not
   retain the popped pointer. The measurements say the prize is another
   order of magnitude, and it is the same kind of proof we already do.
2. **Recycle instead of allocating** where that proof fails: a free list
   handed back from consumer to producer, which is safe precisely because
   single-producer single-consumer has been proved. No new analysis.
3. **Follow queues through members and the heap**, which is what would let
   the analysis speak about ordinary programs rather than about programs
   written with globals.
4. **Elide an uncontended mutex**, the cheapest second demonstration: the
   same ownership analysis, and the mutex path already works end to end.

The order is the measurements' order, not a plan's: the first two are where
the remaining time is, and the third is where the remaining programs are.
