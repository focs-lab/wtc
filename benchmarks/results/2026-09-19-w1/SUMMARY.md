# w1  (2026-09-19T05:43:15Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git b2945a0807a0 (dirty)

Control (shim_generic E64, median of 3 and 3): open 1.10, close 1.17 Mops/s, drift 5.8% — opening controls low, repetitions flat: a warm-up that ended early, not drift; the leg is usable

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.010, rep 2 1.036, rep 3 1.009, rep 4 1.021 (n=43) — no trend.  Widest single row: ring_inline E8, 55% across repetitions.

## W1  E256  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| rigtorp_inline | 5 | 50.38 (49.99–50.47) | 33.48× | 1.04× |
| ring_inline | 5 | 48.60 (47.25–48.91) | 32.30× | 1.00× |
| moodycamel_rw_inline | 5 | 9.48 ⚠ (8.76–10.65) | 6.30× | 0.20× |
| rigtorp_sp | 5 | 3.64 ⚠ (3.23–3.75) | 2.42× | 0.07× |
| ring_sp | 5 | 3.23 ⚠ (2.97–3.68) | 2.15× | 0.07× |
| moodycamel_rw_sp | 5 | 2.84 ⚠ (2.67–3.08) | 1.89× | 0.06× |
| shim_spsc | 5 | 2.80 ⚠ (2.54–3.02) | 1.86× | 0.06× |
| boost_spsc_sp | 5 | 2.35 ⚠ (2.19–2.55) | 1.56× | 0.05× |
| std_mutex | 5 | 1.98 ⚠ (1.92–2.14) | 1.32× | 0.04× |
| moodycamel_cq_sp | 5 | 1.87 ⚠ (1.81–2.01) | 1.24× | 0.04× |
| boost_ms_queue | 5 | 1.73 ⚠ (1.70–1.99) | 1.15× | 0.04× |
| orig_generic | 5 | 1.57 ⚠ (1.54–1.75) | 1.04× | 0.03× |
| shim_generic | 5 | 1.50 ⚠ (1.45–1.77) | 1.00× | 0.03× |

## W1  E64  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 151 (149–151) | 132× | 1.00× |
| rigtorp_inline | 5 | 142 (138–142) | 125× | 0.94× |
| moodycamel_rw_inline | 5 | 10.02 ⚠ (9.19–11.67) | 8.81× | 0.07× |
| rigtorp_sp | 5 | 3.76 ⚠ (3.30–4.09) | 3.31× | 0.02× |
| ring_sp | 5 | 3.47 ⚠ (3.35–3.67) | 3.05× | 0.02× |
| shim_spsc | 5 | 3.28 ⚠ (3.18–3.47) | 2.89× | 0.02× |
| moodycamel_rw_sp | 5 | 3.02 ⚠ (2.90–3.56) | 2.66× | 0.02× |
| boost_spsc_sp | 5 | 2.63 ⚠ (2.39–3.02) | 2.31× | 0.02× |
| moodycamel_cq_sp | 5 | 2.55 ⚠ (2.46–2.70) | 2.24× | 0.02× |
| boost_ms_queue | 5 | 1.90 ⚠ (1.67–1.99) | 1.67× | 0.01× |
| std_mutex | 5 | 1.28 ⚠ (1.23–1.43) | 1.12× | 1/118 |
| orig_generic | 5 | 1.14 ⚠ (1.08–1.19) | 1.00× | 1/132 |
| shim_generic | 5 | 1.14 ⚠ (1.08–1.18) | 1.00× | 1/132 |

## W1  E64  wait=yield

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 3 | 151 (150–152) | 136× | 1.00× |
| shim_spsc | 3 | 3.44 ⚠ (3.32–3.67) | 3.09× | 0.02× |
| orig_generic | 3 | 1.19 ⚠ (1.07–1.29) | 1.07× | 1/126 |
| shim_generic | 3 | 1.11 ⚠ (1.10–1.22) | 1.00× | 1/136 |

## W1  E8  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| rigtorp_inline | 5 | 232 ⚠ (194–251) | 204× | 6.51× |
| ring_inline | 5 | 35.57 ⚠ (26.67–41.38) | 31.28× | 1.00× |
| moodycamel_rw_inline | 5 | 17.43 ⚠ (15.87–18.56) | 15.32× | 0.49× |
| rigtorp_sp | 5 | 3.78 ⚠ (3.51–4.12) | 3.33× | 0.11× |
| ring_sp | 5 | 3.73 ⚠ (3.22–4.00) | 3.28× | 0.10× |
| shim_spsc | 5 | 3.52 ⚠ (3.04–3.76) | 3.09× | 0.10× |
| moodycamel_rw_sp | 5 | 3.49 ⚠ (3.30–3.70) | 3.07× | 0.10× |
| moodycamel_cq_sp | 5 | 3.07 (3.00–3.25) | 2.69× | 0.09× |
| boost_spsc_sp | 5 | 2.70 ⚠ (2.48–3.03) | 2.37× | 0.08× |
| boost_ms_queue | 5 | 1.95 ⚠ (1.63–2.22) | 1.71× | 0.05× |
| std_mutex | 5 | 1.28 ⚠ (1.21–1.47) | 1.13× | 0.04× |
| orig_generic | 5 | 1.15 (1.09–1.18) | 1.01× | 0.03× |
| shim_generic | 5 | 1.14 ⚠ (1.09–1.22) | 1.00× | 0.03× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
