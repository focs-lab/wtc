# eval-fusion: when a pipeline should be one loop, 26 October 2026

`campaign.sh eval-fusion` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors, 5 repetitions under
glibc: W12, three stages of 0 to 200 ns of work each (calibrated once,
1.029 rounds a ns), either on three threads joined by the SPSC ring
(`pipeline`) or fused into one loop on one thread (`fused`). Nothing
allocates. 18:09 to 18:11 local; at every grain both variants printed one
and the same checksum.

Frequency not fixed: 3.92 to 4.12 GHz per run (median 4.10). The leg was
not quiet: outside its set the processors were busy 0.00 to 0.41 of the
time per run (median 0.38; 17 runs near 0.10-0.16), another lane's work
on socket 0, above the 0.10 a timed leg allows. Control drift 0.5%, no
trend; every cell's five repetitions lie within 3%.

## What it shows

Pipeline over fused, paired, M records a second:

| work per stage (ns) | 0 | 2 | 5 | 10 | 20 | 40 | 60 | 100 | 200 |
|---|---|---|---|---|---|---|---|---|---|
| fused | 2,827 | 577 | 229 | 88.6 | 30.5 | 11.6 | 6.78 | 3.75 | 1.75 |
| pipeline | 84.8 | 72.4 | 61.7 | 51.1 | 34.2 | 20.6 | 14.5 | 9.19 | 4.78 |
| pipeline / fused | 0.03 | 0.13 | 0.27 | 0.58 | 1.12 | 1.77 | 2.14 | 2.45 | 2.73 |

- The crossover lies between 10 and 20 ns of work per stage, about 18 ns
  by interpolation: below it fusing the stages is faster, by up to 33×
  with no work, where the pipeline is bound by its two hand-overs (11.8 ns
  a record); above it the pipeline wins, towards the 3× of its three
  cores (2.73× at 200 ns).
- The nominal work is a dependent chain per record. The fused loop
  overlaps consecutive records' chains, so at small grains it runs faster
  than the nominal sum (11.3 ns a record at 10 ns a stage, against 30).

## How far it holds

- A microbenchmark with stages of exactly equal work; a real pipeline's
  slowest stage sets its rate, and fusion then pays only up to that
  stage's share.
- The pipeline spends three cores where the fused loop spends one; per
  core the fused loop is ahead at every grain measured (the pipeline
  reaches 2.73× on three cores).
- Frequency not fixed; the ratios are what this leg reports.

## What it means for the compiler

Fusing a pipeline's stages is not a transformation to apply whenever it
is legal: it is a 2.5× loss from about 100 ns a stage and a large gain
under 20 ns. Like coarsening (W8), it needs an estimate of the work
between the operations it would remove; the crossover is the threshold
such an estimate has to resolve.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. The runtime and W12 are those of main
from a19428f to 7b40685 (compared file by file); driven by campaign.sh at
a19428f (`unpinned.tsv` with a header, as described in
`2026-10-26-eval-pool/NOTE.md`). `checksums.txt` lists each run's
checksum.
