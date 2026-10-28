# Do the checks read the facts the specialisation established? (rerun)

The measurement of `2026-10-26-two-uses`, repeated on main abe26c6, after
three changes to the answers the progress check reads. The first is the
answer to whether code may block, now one query for the progress check
and both batching passes: it resolves pointer parameters only in internal
functions and does not follow replaceable bodies. The second is the
single consumer's pop, which now steps past an unfilled cell and may
allocate. The third is the lock-free queue for one producer, which
replaced the ring whose consumers took turns. As before, every function
declared lock-free (`WTC_LOCK_FREE`) is checked twice on the same CIR,
with the pipeline `tools/wtc-cc` runs: once with the tags rewritten (the
specialised build) and once with them left alone (`--no-specialize`, the
general build). Then the functions whose verdicts differ are counted.

The run is untimed (verdicts only). wtc-opt was built at abe26c6 with the
pinned toolchain. The script, the jobs and the four real-code CIRs are
the same as on 26 October (SHA-256 prefixes: `l2fwd.c.cir`
1c99e019edb09e61, `ring_fast.c.cir` 4f032abeed9c4d7e,
`MonitorNode.cpp.cir` d3fa685474f9b98f, `bench.cpp.cir`
da2c12d4be8ebf88):

    benchmarks/scripts/two_uses.sh verdicts.tsv \
      < benchmarks/results/2026-10-28-two-uses/jobs.txt

`jobs.txt` alone gives the 48 rows from the repository. The real-code
rows need the four commented jobs at its end, as on 26 October.

## What it shows

58 functions are declared lock-free, and 3 change their verdict between
the two builds. They are the same three as on 26 October, all from fail
in the general build to pass in the specialised one. None of the ten
functions in real code changes.

| sources | functions, 26 → 28 Oct | verdict differs, 26 → 28 Oct |
|---|---|---|
| the check's own tests (`test/Transforms/Progress`, four files) | 38 → 42 | 0 → 0 |
| `examples/progress_consumer.cpp` and its `-DRETAIN`, `-DLOG`, `-DPRODUCER` variants | 5 → 5 | 2 → 2 |
| `examples/transfer_pipeline.cpp` | 1 → 1 | 1 → 1 |
| real code: DPDK's l2fwd forwarding loop and five ring fast paths, Cinder's audio `MonitorNode::process`, spdlog's producer and worker (3) | 10 → 10 | 0 → 0 |
| all | 54 → 58 | 3 → 3 |

## Row by row against 26 October

None of the 54 rows of 26 October changes its verdict, in either build.
The four new rows are tests that the shared blocking answer added to
`reasons.cpp`. Each has the same verdict in both builds:
`through_static(void (*)())` passes, while `through_extern(void (*)())`,
`guarded()` and `weakly()` fail.

To tell what the compiler changed from what the sources changed, the
26 October main (7b40685) was built again. On its own sources it
reproduces the 26 October `verdicts.tsv` byte for byte. On today's
sources it differs from today's compiler in two rows, both of them new
and both in both builds:

| function | 7b40685 | abe26c6 | cause |
|---|---|---|---|
| `through_extern(void (*)())` | pass | fail | A pointer parameter is now resolved from the calls seen here only when the function is internal (`9d4d8ef`, PR #42). Another file may pass this one a pointer that blocks, so the call is now "an indirect call whose target is not known here". |
| `weakly()` | pass | fail | A weak body may be replaced at link time, and it is no longer followed (`0476a51`, PR #42). |
| `guarded()` | fail | fail | Only the reason changes: "throws or takes a static-initialisation lock" becomes "takes a static-initialisation lock" (`0476a51`). |
| `through_static(void (*)())` | pass | pass | It is internal, so the pointer is resolved as before. |

Apart from those rows, two reasons changed, both in the specialised build
of `queue-impls.cpp`, and neither verdict changed:

- `take_in()` pops from the queue for many producers. Its pop now also
  "may allocate to note the cells it passes that a push has taken and
  not filled" (`acac0fe`, PR #46). It failed before for freeing the
  segments it moves past, and that reason still holds.
- `take_out()` pops from the queue for many consumers. "Waits for
  another consumer's pop under the consumers' spin lock" becomes "frees
  the segments it moves past" (`778cc97`, PR #48). The pop's progress
  answer went from blocking to lock-free, and a function declared
  lock-free may reach a lock-free operation. But the pop still frees
  segments, and the shared query counts an operation that may call the
  allocator as a blocking site, just as it counts one that waits
  (`BlockingQuery::siteAt` in `mlir/lib/WTC/Transforms/Blocking.cpp`).
  So the function fails as before.

Every other diagnostic of every job reads the same on both dates, call
chains included, apart from line numbers in the two test files that
grew. That includes the ten real-code functions, and the specialisation
chose the same tag for every queue. So the answers that changed since
26 October moved reasons, not verdicts. The three functions whose
verdicts differ between the builds owe the difference to the spsc_inline
ring with every pop borrowed, and that ring's answers did not change. The
two implementations whose answers did change, the queues for many
producers and for many consumers, still fail a consumer step declared
lock-free, because the pops of both free segments.

## spdlog's CIR

This corrects the 26 October note. The spdlog CIR in the real-code set
is not the plain port. The copy of the port it was emitted from has an
injection point (`WTC_INJECT`, the first statement of
`wtc_bound::over()` in its `wtc_q.h`), and in this CIR that point holds
a one-microsecond `std::this_thread::sleep_for`. It was most likely left
there by the runs that injected a blocking call into each program: they
write a file of the same name, and the sleep is the last of them. The
26 October note gives as the reason for the producer's enqueue that its
"bound sleeps while the queue is full"; that sleep is the injected one.
The port's own bound spins on a pause instruction.

The port was emitted again from this tree with nothing injected
(`spdlog-bench-clean` in `jobs.txt`, SHA-256 prefix 15d66464f458119c).
The three functions keep their verdicts and fail in both builds, and
`sink_it_` and `worker_loop_` keep their reasons. `post_async_msg_`,
however, now fails on the queue itself, through each of its three
overflow policies: the push "allocates a new segment when the current
one is full: the queue is unbounded". It also fails on `__assert_fail`
for an unknown policy. The queue's tag is chosen at run time (the queue
for many producers when one worker runs, otherwise the general-purpose
one), and the push of either gives that same answer. So the
implementation cannot change this verdict between the builds either.
The spdlog rows in `verdicts.tsv` come from the 26 October CIR, so that
the rows compare. The control was emitted, from the repository root,
with

    build-llvm/bin/clang++ -std=c++17 -fclangir -emit-cir -w \
      -I<port>/include -Ibenchmarks/harness -Ibenchmarks/queues -Ishim \
      '-DWTC_INJECT=(void)0' benchmarks/apps/spdlog/bench.cpp \
      -o bench-clean.cpp.cir

where `<port>` is the tree that `benchmarks/apps/spdlog/port.py` writes,
with `WTC_LOCK_FREE` on the three functions and the injection point
added.

## What the facts feed, from the code

This is the 26 October table, updated for the shared blocking answer, as
of abe26c6:

| fact | computed by | read by |
|---|---|---|
| topology (producers and consumers of each queue) | `ThreadInstances.cpp`, `QueueOwnershipAnalysis.cpp` (the `Topology` verdict, `wtc.topology` on the constructor) | `WTCSpecialize.cpp` (tag rewrite, tag chosen at run time); `QueueOwnershipAnalysis::checkDeclared` (the declared-topology check); `WTCBorrowAnalysis.cpp` (borrowing is analysed for spsc queues only); the progress check, indirectly, through the implementation `WTCSpecialize` records (`#wtc.impl`), whose answers are in `WTCOps.cpp` |
| ownership (every pop only borrows) | `WTCBorrowAnalysis.cpp` (`wtc.borrowed` on pops) | `WTCSpecialize.cpp` (spsc_inline); the shared blocking query (`Blocking.cpp`: a borrowed spsc_inline pop's shared_ptr owns nothing, so its destructor is not followed), and through it every pass that asks the query: the progress check and both batching passes |
| what may block | `BlockingQuery` (`Blocking.h`, `Blocking.cpp`): the operations' `ProgressOpInterface` answers per implementation (`WTCOps.cpp`), functions without a body classified by name, and calls followed into final bodies only | `WTCCheckProgress.cpp` (every kind but a call that may throw counts) and `WTCBatchPushes.cpp`: the push batching, which accepts the allocator and sleeping, and the lock batching, which accepts the allocator; each also keeps refusals of its own |

On 26 October, what may block had one reader, the progress check, and
batching ran its own walk over calls. Since the shared answer (PR #42,
restored by PR #50), the progress check and the legality of batching
read the same fact.

## What it means for the claim

The claim is that one representation serves both the rewrite and the
check. The measured coupling is unchanged: the specialisation's facts
decide 3 of 58 verdicts, all in examples written to show it, and none of
the 10 in real code. The real code does not show it, for three reasons.
DPDK's ring paths and Cinder's callback use no wtc queue. l2fwd fails on
its logging. spdlog's queue has its tag chosen at run time, the push of
either candidate gives the same answer, and its other failures do not
depend on the queue. What has grown is the reach of one fact: what may
block is now one answer, which the check and the legality of a
transformation both read. The coupling between a fact and a check's
verdict remains a mechanism shown on examples. It has not been measured
as an effect on real code.

## How far it can be trusted

The verdicts are deterministic. The 26 October compiler, built again,
gives the 26 October table exactly, so the comparison above is between
two compilers, not between two runs. As on 26 October, the count of
functions that change is a lower bound on coupling (a check can read a
fact without its verdict changing). The two changed reasons above are
examples of exactly that: the spmc pop's answer changed and the verdict
did not. The real-code CIRs are the files of 26 October, checked by the
checksums above. The spdlog one carries the injected sleep described
above and the shim of 26 October. The control rules out both, since the
same port, emitted with this tree's shim and nothing injected, gives the
same verdicts.
