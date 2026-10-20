# benchmarks

Workloads representative of what the project targets, plus the harness that
measures them.

**Measure the tail, not the mean.** The whole motivation rests on p99 and
p99.9 latency; an average can hide exactly the effect being chased.

**Pin the processors a run owns and let nothing else run on them for its
duration**, or the tail measures the neighbours rather than the code.

**Baselines** are the general-purpose libraries a developer would otherwise
reach for, and the hand-written specialised implementation from `runtime/`.
The second one is the ceiling: the compiler cannot beat code a human wrote
for this exact case, it can only reach it without the human.

## Layout

```
CMakeLists.txt        standalone, system compiler, no LLVM
queues/
  adapters.hpp        one push/try_pop shape over every queue under test
  elements.hpp        E8, E64, E256: sequence number, timestamp, padding
  generic_original.hpp  the shim's queue as it was before the topology tag
harness/harness.hpp   pinning, TSC, percentiles, throttle, results lines
workloads/
  w1_throughput       one producer, one consumer, elements per second
  w2_latency          push-to-pop distribution, open loop and ping-pong
  w3_pipeline         three stages, two queues, through the adapters
  w3_pipeline_wtc     the same pipeline using the queues directly, which is
                      what the compiler can reason about
  w5_mpmc_control     two producers and two consumers: the fallback
  w6_ring_shape       the ring's block size and padding, one per process
  w7_batch_wtc        a producer loop's pushes, batched by the compiler or not
  w8_lock_batch_wtc   a loop's critical sections, held as one by the
                      compiler or not, with one or two threads
scripts/
  check_env.sh        refuses to measure in an unfit environment
  campaign.sh         one leg: interleaved reps, control runs, results dir
  summarise.py        runs.jsonl -> SUMMARY.md with medians, CIs, ratios
  in_bench.py         drives a reservation without a terminal
  build_wtc_flavours.sh  the generic, hand-tagged and compiler-tagged builds
  build_w7_flavours.sh   W7 for each implementation, batched and not
  build_w8_flavours.sh   W8 as written and with its sections batched
  correctness.sh      sanitizer and contract-check leg
third_party/          vendored baselines, pinned commits, licences
                      (boost and oneTBB come from system packages instead)
results/              committed: percentiles and summaries, never raw samples
```

## Variants

| name | what | role |
|---|---|---|
| `orig_generic` | `shim/queue.hpp` as it was, verbatim | what the program pays today; proves the tag branch is free |
| `shim_generic` | the shim, tag `generic` | B0, the denominator of every ratio |
| `shim_spsc` | the shim, tag `spsc` | what the compiler produces when it proves SPSC |
| `ring_sp` | `runtime/spsc_ring` with `shared_ptr<T>` slots, no shim | prices the shim's branch and wrapper |
| `ring_inline` | `runtime/spsc_ring` with `T` inline, 64K-slot blocks | our ceiling: what the shared_ptr in the API costs |
| `std_mutex` | `std::queue<shared_ptr<T>>` under one mutex | the typical developer's queue |
| `moodycamel_rw_sp` / `_inline` | moodycamel ReaderWriterQueue | the closest hand-written analogue, with and without the shared_ptr |
| `moodycamel_cq_sp` | moodycamel ConcurrentQueue | the general-purpose lock-free library one reaches for when the topology is unknown |
| `rigtorp_sp` / `_inline` | Rigtorp SPSCQueue, 64K entries | bounded flat ring; the human ceiling |
| `boost_spsc_sp` | `boost::lockfree::spsc_queue` | classic bounded SPSC |
| `boost_ms_queue` | `boost::lockfree::queue` | Michael-Scott MPMC, for the fallback control |
| `onetbb_cq_sp` | `tbb::concurrent_queue` | the library `docs/vision.md` names; present only where oneTBB is installed |

Every `_sp` variant stores `shared_ptr<T>` and copies `*p` out on pop, so
all of them pay the same per-element allocation and the same copy, and a
difference between them is a difference in the queue. The `_inline` variants
are the ceiling.

## Workloads

- **W1** `w1_throughput`: one producer, one consumer, 20 M elements, E8/E64/
  E256, spin or yield on empty. Elements per second between the first push
  after warm-up and the last pop. An in-flight throttle (at most 4096
  queued) keeps unbounded queues from measuring the allocator instead; the
  bounded baselines see the same limit.
- **W2** `w2_latency`: open loop, the producer sends one element every
  `--interval-ns` (1 us and 10 us in the gate's legs) by busy-waiting on the
  TSC. Latency runs from the time each element was due, not the time it was
  sent: a producer that falls behind its schedule charges the delay to the
  elements it held up instead of hiding it (coordinated omission), and
  `svc_p50`/`svc_p99`/`svc_p999`, from the send, are the queue's own share.
  Each line records the offered and achieved rates and marks a run that did
  not sustain the offered rate `saturated`: such a run measures a backlog.
  p50/p90/p99/p99.9/p99.99/max from one million samples after warm-up.
  `--pingpong` measures half a round trip over two queues as a cross-check
  that does not depend on two cores agreeing about the counter.
- **W3** `w3_pipeline` and `w3_pipeline_wtc`: three stages and two queues,
  with a checksum as the output oracle. The second uses the queues directly
  rather than through an adapter: every adapter builds its queue in its own
  constructor, which serves both queues of a pipeline, and one tag cannot
  serve two queues, so the adapter shape cannot show the compiler's own
  decision. Both have the same in-flight bound, and in the open loop both
  measure latency from each record's due time and mark a run that fell
  behind the offered rate `saturated`.
- **W5** `w5_mpmc_control`: two producers and two consumers. The case where
  the compiler must refuse, so the question is whether the fallback costs
  anything. `--force-unsafe`, with `WTC_CHECK`, runs the single-producer ring
  under two producers on purpose: the negative control.
- **W6** `w6_ring_shape`: block size and cache-line padding, which are
  template parameters rather than options, so one configuration per process.

Every results line also records the process's peak resident set
(`peak_rss_kib`). The latency workloads record the whole distribution as
well as its percentiles (`hist_ns`: log-linear buckets, eight per power of
two, as `lower:count` pairs), and W1 the cost of one element
(`ns_per_op`). Latency timestamps are fenced on both sides, so a sample
brackets exactly the operation.

oneTBB is optional. Where `libtbb-dev` is installed CMake finds it and the
`onetbb_cq_sp` variant appears; where it is not, the configure step says so
and everything else builds and runs unchanged. Nothing else in the tree
depends on a system package beyond boost.

## Protocol

Never build inside a reservation, and never measure outside one.

```sh
# build, capped and off the measured cores
machine-lock --lane wtc --cores 8 --mem 8G --why 'bench build' -- \
  cmake --build build-bench -j8

# reserve two physical cores with their SMT siblings offlined, then run a leg
bench -c 2 -m 4G -e
benchmarks/scripts/campaign.sh w1 build-bench 10
```

Legs: `w1`, `w2`, `w2pp`, `w3`, `w3wtc`, `w5`, `w6`, `smt`, `far`, `tbb`,
`w1check`, and `smoke`, which runs everything once, tiny, unreserved.

`campaign.sh` checks the environment first (governor `performance`, turbo
off, enough cores for the leg), writes `meta.json`, discards a warm-up run,
takes three control runs before and three after, and interleaves variants
round-robin across repetitions so drift hits all of them equally. Each run
is a fresh process and takes the host's exclusive measurement lock for its
own duration.

The summary carries two drift checks, because the obvious one lies on its
own. Comparing single control runs of a variant whose own run-to-run spread
is about 8% flags every leg and means nothing, so the controls are compared
as medians of three against a 5% threshold. Beside them sits the check that
does the work: every configuration normalised to its own first repetition,
tens of ratios rather than two, flagged only when they move in one
direction, since a spread either side of the first is noise and a climb is
drift. Rows whose coefficient of variation exceeds 3% are marked.

A run's element count is chosen per variant. A fixed count made the slowest
queue take 17 seconds and the fastest a tenth of a second, and a tenth of a
second measures process startup rather than the queue.

The measured cores come from `taskset -p $$`, lowest first, on distinct
physical cores; `--smt-siblings` picks a core and its sibling instead, and
`--cpus a,b` overrides both.

## What the numbers mean

Three build flavours of any program: **generic** (source unmodified, tag
default), **hand** (source edited to pass `topology::spsc`), **compiler**
(source unmodified, the tag set by `wtc-opt`'s verdict). All three go through
the same pipeline, otherwise a comparison would measure two compilers rather
than the substitution. The claims and the comparisons that carry them:

| claim | comparison |
|---|---|
| the prize is real | hand vs generic on W1, W2 |
| the substitution is automatic | the compiler's binary and the hand-tagged one are identical bytes, and both differ from the untouched build |
| the fallback is safe and free | compiler == generic where the verdict is not SPSC |
| the ring is competent | `ring_sp` vs `moodycamel_rw_sp`, `ring_inline` vs `rigtorp_inline` |
| the shared_ptr is the next cost | `ring_sp` vs `ring_inline` |

Results are committed under `results/<date>-<leg>/` as percentiles and a
generated `SUMMARY.md`; raw sample arrays are never committed.
