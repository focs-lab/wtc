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

## Running it

Not in CI: shared runners are too noisy for timings to mean anything. Run it
by hand, on a quiet machine, against the pinned toolchain:

```sh
LLVM_BUILD=<pinned toolchain> WTC_BUILD=<this project's build tree> benchmarks/run.sh
```

It takes about two minutes at the default `RUNS=5` on an M1 Pro and
overwrites [`RESULTS.md`](RESULTS.md): medians, latency percentiles, the
spread over runs, and the commit, toolchain and machine they came from. To
keep a result, commit `RESULTS.md` on its own after the run, so the commit
it names is the one that was measured. `RESULTS` writes the report
elsewhere; `BENCH_CPUS` pins every run to those CPUs with `taskset` on
Linux.

## How it works

Each workload in `workloads/` is one source built in up to three variants:

- **baseline**: `wtc::queue<T>`, lifted and lowered back unchanged, so it
  goes through the same pipeline as optimized and differs only in the
  substitution;
- **optimized**: the same, with `--select-queue-impl`;
- **direct**: `wtc::runtime::spsc_queue<T>` used by hand, with its
  allocation-free pop (`-DWTC_BENCH_DIRECT`). The ceiling.

`run.sh` holds, for each workload, whether the optimized build must be
substituted, and fails if it was not, or was when it must not be. A number
in the results therefore always measures what its column says. Every
workload also checks what it received, and fails rather than reports if
anything was lost, duplicated or reordered. Variants run interleaved so
that drift over a session affects them alike.

The queue in each workload is a global touched only by `push` and
`BENCH_POP` at the call sites (`bench.hpp`). Passing it to a helper would be
a use `--select-queue-impl` cannot retarget, and the pass would rightly
refuse to substitute.

| Workload | What it measures |
| --- | --- |
| `spsc_throughput` | one producer, one consumer, 5M ints, flat out |
| `spsc_latency` | push-to-pop latency at a paced 1M items/s: p50, p99, p99.9, max |
| `spsc_payload` | throughput with a 64-byte record as the element type |
| `spsc_same_thread` | one thread pushes a 200k batch and pops it, 10 times: raw cost, no contention, and a case a bounded ring would deadlock on |
| `mpsc_control` | two producers: must not be substituted; measures the noise floor |

## Reading the results

`mpsc_control` is the yardstick. Its two builds run the same queue code (the
only difference is that the optimized one has had the shim's anchor erased),
and on the M1 Pro they still measured 10 to 13 percent apart, in either run
order. A difference that size between any two columns is not evidence of
anything. `RESULTS.md` states the figure for each run.

The gap between optimized and direct is mostly one allocation per item. The
substitute keeps `wtc::queue<T>::try_pop`'s interface, which returns a
`std::shared_ptr<T>`, so every pop builds one; the hand-written consumer
uses `try_pop_optional` and allocates nothing. Closing it means changing
what the dialect's pop returns, not the queue.

## Not measured yet

- Pinned runs on Linux. On macOS threads cannot be pinned, so latency
  includes whatever the scheduler does.
- Latency under bursts or saturation; `spsc_latency` is one steady rate.
- Anything with more than one queue, or with queues that are not globals,
  since those are not substituted.
- Memory: the substitute allocates in blocks, the original per item.
