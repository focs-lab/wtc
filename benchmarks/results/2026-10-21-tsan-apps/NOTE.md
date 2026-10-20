# The applications under ThreadSanitizer, 21 October 2026

The compiler's builds of the two ported applications, made with
`TSAN=1` by `benchmarks/apps/{dedup,ferret}/build.sh` at 9263957, run
under ThreadSanitizer (`halt_on_error=1`) in the evaluation's container.
dedup is instrumented in every file; ferret in the translation unit with
its queues and threads, its libraries (which start no threads) not.
Each run's output is compared with PARSEC's own build on the same input.

| program | build | threads per stage | input | ThreadSanitizer reports | output |
|---|---|---|---|---|---|
| dedup | compiler (queues general: arrays) | 1, 2, 4 | simdev | 0 | PARSEC's |
| dedup | compiler (queues general: arrays) | 1, 2, 4 | first 32 MB of native | 0 | PARSEC's |
| dedup | one translation unit, globals (three inline rings, one general) | 1 | simdev | 0 | PARSEC's |
| dedup | one translation unit, globals (three inline rings, one general) | 1 | first 32 MB of native | 0 | PARSEC's |
| ferret | compiler, one thread per stage (four inline rings, one general) | 1 | simdev | 0 | PARSEC's |
| ferret | compiler, threads from argv (all general) | 2 | simdev | 0 | PARSEC's, line for line |

That the instrumented build would report a race the substitution caused
is what `test/Integration/tsan_own_build_caught.cpp` checks: a
single-producer ring forced onto two producers is reported.
