# w1  (2026-09-18T18:19:35Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;`, git fd0438b3ba9b (dirty)

Control (shim_generic E64, median of 1 and 1): open 0.97, close 1.08 Mops/s, drift 11.0% — opening controls low, repetitions flat: a warm-up that ended early, not drift; the leg is usable

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.007, rep 2 0.998, rep 3 1.010, rep 4 1.005 (n=43) — no trend.  Widest single row: orig_generic E256, 49% across repetitions.

## W1  E256  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| rigtorp_inline | 5 | 50.36 (49.81–51.55) | 32.37× | 1.06× |
| ring_inline | 5 | 47.65 ⚠ (42.37–48.15) | 30.63× | 1.00× |
| moodycamel_rw_inline | 5 | 9.01 ⚠ (8.48–10.49) | 5.79× | 0.19× |
| rigtorp_sp | 5 | 3.40 ⚠ (3.21–3.74) | 2.18× | 0.07× |
| ring_sp | 5 | 3.05 ⚠ (2.81–3.55) | 1.96× | 0.06× |
| moodycamel_rw_sp | 5 | 2.99 ⚠ (2.69–3.14) | 1.92× | 0.06× |
| shim_spsc | 5 | 2.72 ⚠ (2.65–3.02) | 1.75× | 0.06× |
| boost_spsc_sp | 5 | 2.32 (2.19–2.34) | 1.49× | 0.05× |
| std_mutex | 5 | 2.03 ⚠ (1.86–2.08) | 1.31× | 0.04× |
| moodycamel_cq_sp | 5 | 1.98 ⚠ (1.88–2.05) | 1.27× | 0.04× |
| boost_ms_queue | 5 | 1.65 ⚠ (1.63–1.81) | 1.06× | 0.03× |
| orig_generic | 5 | 1.65 ⚠ (1.36–2.02) | 1.06× | 0.03× |
| shim_generic | 5 | 1.56 ⚠ (1.33–1.63) | 1.00× | 0.03× |

## W1  E64  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 148 (146–149) | 137× | 1.00× |
| rigtorp_inline | 5 | 139 (136–141) | 129× | 0.94× |
| moodycamel_rw_inline | 5 | 10.52 ⚠ (9.28–11.21) | 9.74× | 0.07× |
| rigtorp_sp | 5 | 4.14 ⚠ (3.61–4.40) | 3.84× | 0.03× |
| ring_sp | 5 | 3.59 ⚠ (3.18–3.71) | 3.33× | 0.02× |
| shim_spsc | 5 | 3.31 ⚠ (3.11–3.56) | 3.07× | 0.02× |
| moodycamel_rw_sp | 5 | 3.16 ⚠ (2.94–3.88) | 2.92× | 0.02× |
| boost_spsc_sp | 5 | 2.61 ⚠ (2.29–2.91) | 2.41× | 0.02× |
| moodycamel_cq_sp | 5 | 2.54 (2.46–2.60) | 2.35× | 0.02× |
| boost_ms_queue | 5 | 1.93 ⚠ (1.62–2.16) | 1.79× | 0.01× |
| std_mutex | 5 | 1.41 ⚠ (1.28–1.51) | 1.30× | 1/105 |
| orig_generic | 5 | 1.18 (1.14–1.24) | 1.09× | 1/125 |
| shim_generic | 5 | 1.08 ⚠ (1.04–1.23) | 1.00× | 1/137 |

## W1  E64  wait=yield

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 3 | 148 (146–149) | 131× | 1.00× |
| shim_spsc | 3 | 3.83 ⚠ (3.31–4.04) | 3.39× | 0.03× |
| shim_generic | 3 | 1.13 (1.08–1.15) | 1.00× | 1/131 |
| orig_generic | 3 | 1.12 ⚠ (1.07–1.21) | 0.99× | 1/132 |

## W1  E8  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| rigtorp_inline | 5 | 289 ⚠ (282–353) | 250× | 7.85× |
| ring_inline | 5 | 36.85 ⚠ (34.09–51.06) | 31.81× | 1.00× |
| moodycamel_rw_inline | 5 | 16.82 ⚠ (14.11–19.27) | 14.52× | 0.46× |
| rigtorp_sp | 5 | 4.24 ⚠ (3.78–4.29) | 3.66× | 0.12× |
| ring_sp | 5 | 3.57 ⚠ (3.42–4.00) | 3.08× | 0.10× |
| shim_spsc | 5 | 3.51 ⚠ (3.17–3.84) | 3.03× | 0.10× |
| moodycamel_rw_sp | 5 | 3.47 ⚠ (3.29–4.01) | 2.99× | 0.09× |
| moodycamel_cq_sp | 5 | 3.05 (3.03–3.10) | 2.64× | 0.08× |
| boost_spsc_sp | 5 | 2.69 ⚠ (2.48–2.81) | 2.33× | 0.07× |
| boost_ms_queue | 5 | 1.86 ⚠ (1.70–2.24) | 1.61× | 0.05× |
| std_mutex | 5 | 1.37 (1.37–1.45) | 1.18× | 0.04× |
| shim_generic | 5 | 1.16 ⚠ (1.08–1.20) | 1.00× | 0.03× |
| orig_generic | 5 | 1.15 ⚠ (1.07–1.21) | 1.00× | 0.03× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
