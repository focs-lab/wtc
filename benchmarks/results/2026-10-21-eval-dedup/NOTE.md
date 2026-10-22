# eval-dedup: PARSEC dedup, 21 October 2026

`campaign.sh eval-dedup` at 915a601 on focs, eighteen cores with their
SMT siblings offline (processors 4 to 21), governor performance, turbo
off, 10 repetitions, on the native input (the Fedora Core 6 x86_64 install
disc, 704 MB, checked against the archive's SHA-1). Builds: PARSEC's own
(`orig`, condition variables), and the port of `benchmarks/apps/dedup` as
`build.sh` makes it. With one thread a stage: the port as written without
specialisation (`generic-one`, B0) and through the compiler
(`compiler-one`), tagged by hand with inline rings (`hand-one`, B3) or by
pointer (`hand-spsc-one`), and the compiler on the port with its queues
as globals (`compiler-flat-one`) and with, besides, dedup's `main` in the
same translation unit (`compiler-flat-one-unity`). With 2 and 4 threads a
middle stage: `orig`, `generic` and `compiler`. Time is PARSEC's own
region of interest; the output goes to /dev/shm. It ran 18:28 to 19:06
local; the controls (shim_generic, 64 bytes) read 1.06 to 1.20 Mops/s at
the start and 1.07 to 1.15 at the end.

All 130 runs wrote PARSEC's output byte for byte.

| threads a stage | orig | generic | compiler | hand | flat | flat, unity |
|---|---|---|---|---|---|---|
| 1 | 20.97 s | 21.16 | 21.14 | 21.05 (inline), 21.09 (by pointer) | 21.14 | 20.97 |
| 2 | 10.86 | 11.02 | 11.02 | | | |
| 4 | 5.65 | 5.76 | 5.76 | | | |

The compiler's verdicts are the ones `benchmarks/apps/dedup/README.md`
gives: every queue refused as written and as globals, so `compiler-one`
and `compiler-flat-one` build what `generic-one` builds and run within
0.1% of it, and three inline rings and the list for many producers in the
unity build, the tags written by hand in `hand-one` plus the reorder
queue. At 2 and 4 threads every queue is refused and `compiler` equals
`generic`.

The queues move dedup by under 1%. Its chunks average 1.88 KB, about
366,000 of them, so with one thread a stage the run spends about 57 µs a
chunk, the time of its slowest stage; at that grain eval-w3grain puts the
gain of the compiler's queues at a few percent at most, and the port's
six builds span 0.9%. Against PARSEC's own build the port is up to 1%
behind at one thread a stage (level with it in the unity build), 1.5% at
two and 2% at four; the cause has not been isolated (the port waits by
spinning on `try_pop` where PARSEC sleeps on a condition variable, and its
queues are unbounded).

So dedup answers the reach question (RQ1: refused for two reasons the
analysis names, arrays of queues and a program spread over several files,
and exact once both are removed) and the safety question (every build
computes PARSEC's output, and the compiler's runs clean under
ThreadSanitizer in 2026-10-21-tsan-apps), but, like ferret, not the speed
question.
