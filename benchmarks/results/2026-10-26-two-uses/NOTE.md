# Do the checks read the facts the specialisation established?

The claim under test: one IR serves both specialisation and contract
checking, so a check's verdict depends on the facts that chose the
implementation. Measured for the progress check (`--wtc-check-progress`),
by checking every function declared lock-free twice on the same CIR, with
the pipeline `tools/wtc-cc` runs: once with the tags rewritten (the
specialised build) and once with them left alone (`--no-specialize`, the
general build). `benchmarks/scripts/two_uses.sh` does it; `jobs.txt` lists
the repository's sources, `verdicts.tsv` holds every row.

Untimed (verdicts only), on main 7b40685, built with the pinned toolchain.
From any directory, with the tools found as `tools/wtc-cc` finds them
(`TOOLCHAIN` and `WTC_OPT` override):

    benchmarks/scripts/two_uses.sh verdicts.tsv \
      < benchmarks/results/2026-10-26-two-uses/jobs.txt

`jobs.txt` alone gives the 44 rows from the repository; the real-code rows
need the four jobs at its end (see below).

## What it shows

54 functions declared lock-free; 3 change their verdict between the two
builds, all from fail in the general build to pass in the specialised one.

| sources | functions | verdict differs |
|---|---|---|
| the check's own tests (`test/Transforms/Progress`, four files) | 38 | 0 |
| `examples/progress_consumer.cpp` and its `-DRETAIN`, `-DLOG`, `-DPRODUCER` variants | 5 | 2 |
| `examples/transfer_pipeline.cpp` | 1 | 1 |
| real code: DPDK's l2fwd forwarding loop and five ring fast paths, Cinder's audio `MonitorNode::process`, spdlog's producer and worker (3) | 10 | 0 |

The three that change are the examples written to show it. In
`progress_consumer.cpp`, `consume_one()` fails in the general build because
its pop "waits for a push that has taken the cell it reads to fill it, and
frees the segments it moves past, and allocates the shared_ptr it returns";
in the specialised build the queue is proved spsc, every pop only borrows,
the tag becomes spsc_inline, and the same function passes. With `-DRETAIN`
(the consumer keeps the element) or `-DLOG` (it calls printf) it fails in
both builds, for that reason. `transfer_pipeline.cpp` changes the same way
through the transfer form.

None of the ten real-code functions changes. The DPDK ring paths and
Cinder's callback pass in both builds and use no wtc queue; l2fwd fails in
both on its logging. spdlog's three fail in both builds, each for a reason
found before the queue is reached, which hides whatever the queue's
implementation answers: the worker (`worker_loop_`) builds an `async_msg`
for every message, whose `std::promise` allocates its shared state;
the producer's entry (`sink_it_`) locks the logger's `weak_ptr` to the
thread pool, and releasing that `shared_ptr` may run the control block's
destructor, an indirect call the check cannot follow; and the producer's
enqueue (`post_async_msg_`) applies spdlog's overflow policy, whose bound
sleeps while the queue is full (`std::this_thread::sleep_for`), and asserts
on an unknown policy (`__assert_fail`, whose body is not visible).

## What the facts feed, from the code

| fact | computed by | read by |
|---|---|---|
| topology (producers and consumers of each queue) | `ThreadInstances.cpp`, `QueueOwnershipAnalysis.cpp` (the `Topology` verdict, `wtc.topology` on the constructor) | `WTCSpecialize.cpp` (tag rewrite, tag chosen at run time); `QueueOwnershipAnalysis::checkDeclared` (the declared-topology check, from the same verdict); `WTCBorrowAnalysis.cpp` (borrowing is analysed for spsc queues only); the progress check, indirectly, through the implementation `WTCSpecialize` records (`#wtc.impl`), whose answers are in `WTCOps.cpp` |
| ownership (every pop only borrows) | `WTCBorrowAnalysis.cpp` (`wtc.borrowed` on pops) | `WTCSpecialize.cpp` (spsc_inline); `WTCCheckProgress.cpp` (`ProgressChecker::collectExemptions`, line 204, reads `wtc.borrowed` on each pop together with its implementation: a borrowed spsc_inline pop's shared_ptr owns nothing, so its destructor is not followed) |
| what may block | `ProgressOpInterface`, answered per implementation in `WTCOps.cpp` | `WTCCheckProgress.cpp` only. Batching (`WTCBatchPushes.cpp`, `LegalityChecker`) decides whether a loop may wait with its own walk over calls; it does not read the interface |

So the topology fact has two readers that matter for the claim, the
substitution and the declared check, and reaches the progress check
through the implementation it chose. Ownership reaches it both ways: through
that implementation, and directly, since the progress check reads
`wtc.borrowed` itself.
What may block is read by the progress check alone: batching shares the
representation, not the fact.

## How far it can be trusted

The verdicts are deterministic. The count of functions that change is a
lower bound on coupling in principle (a check could also read a fact
without its verdict changing), but it is the observable one, and on the
real code measured here it is zero. The real-code CIRs were emitted from
the sources the progress check was first run on (Cinder at f90f332, DPDK
26.11.0-rc0 at 6bbb7b3, the spdlog 1.14.1 port), with `WTC_LOCK_FREE`
added to the functions named; they are not in the repository, so those
four jobs (ten rows) are commented out at the end of `jobs.txt`, with the
names of the CIRs they read.

The declared-topology check reads the same `Topology` verdict as the
substitution by construction (`checkDeclared` takes it as an argument), so
every declaration it judges depends on the topology fact; its results on
real code are in `docs/declared-topology-audit.md`, part 3.
