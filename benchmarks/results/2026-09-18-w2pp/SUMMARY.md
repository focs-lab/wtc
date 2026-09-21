# w2pp  (2026-09-18T20:21:12Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;`, git 06689f0d99cb (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.020, rep 2 1.047, rep 3 0.962, rep 4 1.012 (n=4) — no trend, but a wide spread (8.5%).  Widest single row: ring_inline E64, 32% across repetitions.

## W2 pingpong  E64  wait=spin

| variant | n | p50 | p90 | p99 | p99.9 | p99.99 | max | p99 vs shim_generic | p99.9 vs shim_generic |
|---|---|---|---|---|---|---|---|---|---|
| ring_inline | 5 | 261 | 307 | 358 ⚠ (324–419) | 1,801 | 3,702 | 39,071 | 0.10× | 0.36× |
| moodycamel_rw_sp | 5 | 372 | 415 | 439 ⚠ (390–492) | 582 | 3,806 | 22,818 | 0.13× | 0.12× |
| shim_spsc | 5 | 340 | 412 | 475 (465–492) | 611 | 3,814 | 22,174 | 0.14× | 0.12× |
| shim_generic | 5 | 1,427 | 1,780 | 3,489 ⚠ (2,045–3,714) | 5,042 | 6,993 | 36,145 | 1.00× | 1.00× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
