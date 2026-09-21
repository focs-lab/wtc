# w6  (2026-09-18T20:36:07Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;`, git a778ce08b691 (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 7.512, rep 2 3.784, rep 3 7.482, rep 4 7.425 (n=9) — no trend, but a wide spread (651.2%).  Widest single row: inline/256/64 E64, 1314% across repetitions.

## W6  E64

| variant | n | Mops/s |
|---|---|---|
| inline/4096/64 | 5 | 60.48 ⚠ (7.34–65.71) |
| inline/4096/128 | 5 | 58.27 ⚠ (8.84–66.39) |
| inline/16384/64 | 5 | 57.13 ⚠ (7.84–61.93) |
| inline/256/128 | 5 | 53.21 ⚠ (6.21–57.68) |
| inline/256/64 | 5 | 52.67 ⚠ (5.13–72.56) |
| inline/1024/64 | 5 | 52.03 ⚠ (7.01–61.52) |
| inline/64/64 | 5 | 8.19 ⚠ (5.91–9.19) |
| shared_ptr/4096/64 | 5 | 3.39 (3.29–3.53) |
| shared_ptr/256/64 | 5 | 3.16 ⚠ (2.54–3.35) |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
