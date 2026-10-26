# false sharing where it really exists, 26 October 2026

A paired run (`run.sh`, 20 rounds, every configuration once a round in a
fresh shuffled order) on the second host, a two-socket AMD EPYC 9115,
socket 1's processors; frequency not fixed (median 3.4 to 4.1 GHz by
configuration), outside-busy 0.005 to 0.023. `runs.tsv` holds every run;
`analysis.txt` is `analyse.py`'s output: medians, spread, frequency, and
the ratio of two builds within each round with a bootstrap 95% interval.
16:13 to 16:20 local. The binaries were static builds made from main at
c010815 and are not committed; how each was made is below. `analyse.py .`
re-reads `runs.tsv` and reprints `analysis.txt`.

## The cases

- W7 before its sample fix (208f666 on main; its consumer appended to a global that shared a
  cache line with `g_interval`, which the producer reads on every
  element): built as it was (pre), the same through the align-by-owner
  pass (prealign), and with the hand fix (fix), on the general-purpose
  queue and the ring with the element in the cell, 8 and 64 B.
- W3 and W2 before their sample fix (bc83907 on main; the sink's and consumer's sample vectors had
  their end pointers in locals of the function that starts the threads,
  captured by the thread lambdas): pre and with the fix; the align pass
  only raises globals, so it has no variant here.
- Phoenix 2.0 linear_regression, the textbook case (per-thread
  accumulators in one calloc'd array): as written and padded by hand to
  128 B per thread, at -O2 and at -O0.

## What it shows

Ratios within a round (above 1 means the second-named is slower):

| comparison | ratio | 95% interval |
|---|---|---|
| linear_regression -O0: padded over as written (elapsed time) | 3.77 | 3.47-3.94 |
| linear_regression -O2: padded over as written | 0.99 | 0.98-1.04 |
| W2 shim spsc_inline: fix over pre (p50 latency) | 1.14 | 1.12-1.17 |
| W7 element in the cell, 64 B: fix over pre (throughput) | 0.96 | 0.96-0.96 |
| W7 element in the cell, 64 B: prealign over pre | 1.01 | 1.01-1.01 |
| W7 general-purpose, 64 B: prealign over pre | 1.01 | 1.01-1.01 |
| W3, every queue: fix over pre | 0.99-1.00 | |

- Real false sharing costs up to 3.8× (linear_regression at -O0); at -O2
  the accumulators stay in registers and it costs nothing.
- On this host the benchmarks' own false sharing cost little: the hand
  fix lowers W2's median latency by 14%, leaves W3 unchanged and makes W7
  with the element in the cell at 64 B 4% slower (0.958); on the Intel host
  where it was found it cost up to 2.4×. The cost depends on the machine.
- The diagnosis (`--wtc-false-sharing-diagnose`) finds the textbook case
  (17 definite pairs in linear_regression) but missed both real cases in
  our own benchmarks: a vector written inside a library call, and stack
  locals captured by thread lambdas. The align pass's 1% on W7 is an
  accident: it raised `g_interval` for another pair, which moved it off the
  culprit's line.

## How the builds were made

W7 through tools/wtc-cc with the workload source before 208f666's W7 hunk (pre and
prealign, the latter adding `--wtc-align-by-owner`) or at main (fix); W3
and W2 by the benchmarks' CMake build with bc83907's W2 and W3 hunks reversed (pre)
or not; linear_regression from Phoenix 2.0 with a timer around the
threaded phase and, for pad, each per-thread struct padded to 128 bytes
in a 128-aligned array. All static, built in the toolchain image.
