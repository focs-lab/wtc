# Laned MPSC: what relaxing the order between producers could gain

**Untimed. Not a leg of the evaluation.** Run on the two-socket Intel host
(Xeon Gold 6230, Cascade Lake, 2.1 GHz nominal, turbo on, governor
powersave, frequency not fixed; an unprivileged container, so nothing
could be fixed) on its second NUMA node only (processors 20-39 and their
SMT siblings 60-79), inside the Ubuntu 24.04 image, clang 18.1.3 `-O2
-march=native`. The node was shared with another line of work's builds and
tests, serialised with ours by a lock held for each repetition, so no
build ran during a repetition, but nothing else was kept off the host. Ten
repetitions, each in its own shuffled order. SUMMARY.md has medians with
the minimum and maximum of the ten.

Code: W13 and the laned queue as in the commits "runtime: Add a queue that
keeps only per-producer order" and "benchmarks: Measure MPSC with and
without producer order"; spdlog as in "benchmarks: Build spdlog over the
laned queue" (`LANED=1 build.sh`), spdlog v1.14.1 (27cb4c767086).

## Placement

- Streaming and paced, P = 1 to 16: the consumer on processor 20, producer
  p on 21 + p; every thread on a physical core of its own, no SMT sibling
  in use.
- Sparse and bursty: the consumer on 20, 16 to 192 sleeping producers
  spread over the other 38 hardware threads of the node, SMT siblings
  included (two producers or more to a hardware thread at 64 and 192).
- spdlog: the worker on 20, logging thread t on 21 + t.

## What the variants are

All hold the element in the cell and are popped by moving it out
(`adapters.hpp`, W13): `laned` is `runtime/laned_mpsc.hpp` through the
shim's experimental tag (burst 64, the ring's slip on), `laned, no slip`
the same with the ring's slip off, `seg MPSC` the shim's mpsc tag (the
linearisable single-consumer segmented queue), `seg MPMC` the shim's
general queue, `moodycamel CQ` moodycamel::ConcurrentQueue<T> without
tokens (per-producer FIFO, like the laned queue), `mutex+deque` a
std::deque<T> under one std::mutex.

## Caveats

- In streaming runs latency from push to pop measures the backlog the
  throttle allows (about 8192 elements), so it says which side is the
  bottleneck, not how fast the queue is; it is not reported. The paced
  runs carry the latency.
- The paced runs' p99.9 (0.2-0.7 ms for every variant alike) is the host,
  not the queues.
- `moodycamel CQ` and `mutex+deque` with 192 sleeping producers (and with
  64 producers in bursts) fall behind the offered rate and their latency
  is a backlog of tenths of a second. For the mutex that is the sleeping
  producers' convoy on the lock; moodycamel's consumer, which picks among
  its producers' sub-queues by an estimate of their sizes, did not keep up
  with 192 of them here. Neither was investigated further.

## spdlog's order between producers

The laned build is a ceiling, not a correct substitution: spdlog pushes a
flush message and, at shutdown, a terminate message from a thread after it
has joined the logging threads, and expects them to come out after the
logging threads' messages. On the laned queue they can overtake them, so
`flush` can return before the last messages are written and shutdown can
drop them. The two `-cxx` builds therefore wait for the queue to drain
before the final flush. Built without that wait and run eight times with
a file sink (4 threads x 200000 and 8 threads x 50000 messages), the laned
build wrote every line: the bench spends tens of milliseconds sorting its
samples between the flush and the shutdown, which gives the worker time to
drain. The hazard is shown by the code, not by these runs.
