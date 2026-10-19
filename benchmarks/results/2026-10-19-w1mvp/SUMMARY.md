# w1mvp  (2026-10-19T08:48:06Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-6, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git e6581d1d0e4b (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 0.982, rep 2 0.961, rep 3 1.300, rep 4 1.177 (n=10) — no trend, but a wide spread (34.0%).  Widest single row: ring_inline E64, 1170% across repetitions.

## W1  E64  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_borrowed | 5 | 14.65 ⚠ (10.89–122) | 13.74× | 1.01× |
| ring_inline | 5 | 14.57 ⚠ (11.71–149) | 13.66× | 1.00× |
| shim_spsc_inline | 5 | 7.92 ⚠ (5.81–34.26) | 7.43× | 0.54× |
| shim_spsc | 5 | 2.81 ⚠ (2.62–3.48) | 2.64× | 0.19× |
| shim_generic | 5 | 1.07 ⚠ (1.01–1.15) | 1.00× | 0.07× |

## W1  E8  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 27.64 ⚠ (19.58–44.02) | 26.81× | 1.00× |
| ring_borrowed | 5 | 27.53 ⚠ (16.40–35.55) | 26.70× | 1.00× |
| shim_spsc_inline | 5 | 21.02 ⚠ (19.91–26.07) | 20.39× | 0.76× |
| shim_spsc | 5 | 3.11 ⚠ (2.87–3.65) | 3.01× | 0.11× |
| shim_generic | 5 | 1.03 ⚠ (0.99–1.17) | 1.00× | 0.04× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
