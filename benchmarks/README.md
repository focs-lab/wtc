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

- `queue_bench.cpp`: one producer, one consumer, 5M ints through a global
  `wtc::queue<int>`. It also instantiates `wtc::spsc_queue<int, 1 << 23>` as an
  anchor for `--select-queue-impl` to find. Capacity is set above the item
  count on purpose: the substituted `push` can return false when full, and the
  lowering does not retry yet, so an overflow would drop items.
- `spsc_direct.cpp`: the same workload written directly against `spsc_queue`.
  This is the ceiling.
- `run.sh`: builds the baseline and optimized variants of `queue_bench.cpp`
  from the same source (they differ only in whether `--select-queue-impl`
  runs), builds `spsc_direct`, and runs all three. Output goes to
  `build/benchmarks/`. Set `LLVM_BUILD` and `WTC_BUILD` if your trees are not
  in the default places.

Only throughput is measured so far, not the tail, and runs are not pinned.
First result on an Apple M-series laptop, one run each: baseline 2.65M items/s,
optimized 5.74M items/s, direct 11.7M items/s.
