# Reach and precision, 30 October 2026

`benchmarks/scripts/reach.sh` at eb2e7e8 (ClangIR 1ebf89d), with ferret
and dedup built by their `build.sh` at the same tree, in the evaluation's
container on a-p13. The verdicts are the analysis's, so the machine does
not matter. New since 26 October: the lifted ferret, PARSEC's own sources
with the queue lifted (`ferret-lifted`), whose five queues the oracle now
lists.

48 queues: 36 exact, 12 conservative, 0 refused, **0 wrong**. Of the 25
queues a reader would call single-producer single-consumer, 19 are proved
so. 10 have their tag chosen when the queue is built, from the count of
the loop that starts a side's threads.

Against the 26 October table (`../2026-10-26-reach`), on the 43 queues
both have:

- **ferret's loader queue, both port builds: refused, now exact.** The
  loader pushes from inside its recursive directory scan; the push names
  the queue a global pointer holds, written once, so it names the same
  queue at every depth of the recursion and is counted (docs/lifting.md,
  "Reading the threads of real programs"). With one thread a stage the
  queue is proved spsc; with the middle stages' threads from argv, spmc,
  its consumers chosen at run time.
- Every other row is unchanged.

The lifted ferret's five rows: the three middle queues exact, the first
and last conservative, every one chosen at run time. Its stages start in
PARSEC's thread pool, `tpool_create`, from descriptors whose entries the
analysis now reads back, in a loop over a count main passes; the loader's
and the output stage's counts are globals the program never changes from
1, which the oracle counts as one thread and the analysis as one per
iteration of the pool's loop, so the choice gives the queue the
single-thread side at run time.

The oracle's cause for the port's loader queue, "the loader pushes from
inside its recursive directory scan", no longer applies and is dropped.
