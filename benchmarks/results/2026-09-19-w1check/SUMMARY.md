# w1check  (2026-09-19T05:33:52Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=ON;tbb=1;`, git 5d5454c69159 (dirty)

Control (shim_generic E64, median of 3 and 3): open 0.97, close 1.13 Mops/s, drift 14.7% — **NOISY: rerun this leg**

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.060, rep 2 1.222, rep 3 1.204, rep 4 1.145 (n=8) — no trend, but a wide spread (22.2%).  Widest single row: ring_inline E64, 752% across repetitions.

## W1  E64  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 67.25 ⚠ (8.98–76.53) | 58.12× | 1.00× |
| shim_spsc | 5 | 3.29 ⚠ (2.41–3.64) | 2.84× | 0.05× |
| ring_sp | 5 | 3.23 ⚠ (2.64–3.38) | 2.80× | 0.05× |
| shim_generic | 5 | 1.16 ⚠ (1.07–1.19) | 1.00× | 0.02× |

## W1  E8  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 15.38 ⚠ (12.32–23.26) | 13.77× | 1.00× |
| shim_spsc | 5 | 3.52 ⚠ (3.05–3.80) | 3.16× | 0.23× |
| ring_sp | 5 | 3.42 ⚠ (3.04–3.87) | 3.07× | 0.22× |
| shim_generic | 5 | 1.12 ⚠ (1.01–1.23) | 1.00× | 0.07× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
