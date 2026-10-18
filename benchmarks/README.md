# Benchmarks

Workloads representative of what the project targets, plus the harness that
measures them.

Measure the tail, not the mean. The whole motivation rests on p99 and p99.9
latency; an average can hide exactly the effect being chased.

Pin the processors a run owns and let nothing else run on them for its
duration, or the tail measures the neighbours rather than the code.

Baselines are the general-purpose libraries a developer would otherwise reach
for, and the hand-written specialised implementation from `runtime/`. The
second one is the ceiling: the compiler cannot beat code a human wrote for
this exact case, it can only reach it without the human.

## What is here

- `queue_bench.cpp`: one producer and one consumer move 5M ints through a
  global `wtc::queue<int>`. The consumer checks the count, the sum and the
  order, and the run fails if any is wrong. Nothing in it names the
  substitute: the shim makes `wtc::runtime::spsc_queue<int>` available.
- `spsc_direct.cpp`: the same workload written by hand against
  `runtime/spsc_queue.hpp`, with its allocation-free pop. This is the ceiling.
- `run.sh`: builds `queue_bench.cpp` twice from the same CIR, once as
  written and once with `--select-queue-impl` in the pipeline, plus
  `spsc_direct`, through the pinned toolchain down to a native binary, and
  reports the median of `RUNS` runs of each. Set `LLVM_BUILD` to the pinned
  toolchain (default `/opt/llvm`, as in the image) and `WTC_BUILD` to this
  project's build tree.

Only throughput is measured so far, not the tail, and runs are not pinned.

## Measured

At the pin (`1ebf89d8c5e6`), 2026-10-18, on an Apple M1 Pro (8 cores,
16 GB), macOS, threads not pinned, five runs each, median:

| variant | items/s | vs baseline |
| --- | ---: | ---: |
| baseline: `wtc::queue<int>` | 2.07M | 1.0x |
| optimized: substituted by `--select-queue-impl` | 7.44M | 3.6x |
| direct: `spsc_queue` by hand | 43.1M | 20.9x |

The gap between optimized and direct is almost all one allocation per item.
The substitute has to keep `wtc::queue<T>::try_pop`'s interface, which
returns a `std::shared_ptr<T>`, so every pop builds one; the hand-written
consumer uses `try_pop_optional` and allocates nothing. Closing it means
changing what the dialect's pop returns, not the queue.
