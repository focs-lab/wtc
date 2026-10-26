# Reach and precision, 26 October 2026

`benchmarks/scripts/reach.sh` at 609a532, with ferret and dedup built by
their `build.sh` at the same tree, which adds two dedup builds: the port as
written and the port with one thread per stage, each with `main` in the
encoder's translation unit and the queues still in their arrays. The
verdicts are the analysis's, so the machine does not matter; it ran in the
evaluation's container on an unreserved host.

43 queues: 31 exact, 10 conservative, 2 refused, **0 wrong**. Of the 25
queues a reader would call single-producer single-consumer, 18 are proved
so and specialised.

Against the 21 October table (`../2026-10-21-reach`), every row outside
dedup is unchanged. On the 35 queues both tables have, 21 exact, 4
conservative, 10 refused before; 23 exact, 10 conservative, 2 refused now.
The change is dedup's arrays of queues, now counted as one queue each,
every element together:

- **dedup as written** and **with one thread per stage**: the 8 refusals
  ("the queues are elements of arrays, indexed at run time") become 2
  exact and 6 conservative verdicts. What remains is the cause the flat
  builds showed: the threads start in `Encode`, which only `main`, in
  another file, calls, so from this file `Encode` may run any number of
  times. The analysis names that cause on each conservative row.
- **The same two with `main` in the translation unit** (new): all 8 exact.
  With one thread per stage, three of the four arrays are proved
  single-producer single-consumer and every element is an inline ring,
  as in the build with the queues as globals; the reorder array is
  multi-producer. Both builds compress and decompress the simdev input
  back to itself.

The 2 refusals left are ferret's loader queue, pushed to from inside a
recursive directory scan.
