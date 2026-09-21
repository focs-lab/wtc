# tbb  (2026-09-19T05:09:49Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-7, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git 5d5454c69159 (dirty)

Control (shim_generic E64, median of 3 and 3): open 0.99, close 1.09 Mops/s, drift 9.6% — opening controls low, repetitions flat: a warm-up that ended early, not drift; the leg is usable

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.017, rep 2 1.004, rep 3 0.998, rep 4 0.983 (n=18) — no trend.  Widest single row: boost_ms_queue E64, 38% across repetitions.

## W1  E256  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic |
|---|---|---|---|
| shim_spsc | 5 | 2.81 ⚠ (2.71–2.99) | 1.87× |
| moodycamel_cq_sp | 5 | 2.08 ⚠ (1.85–2.16) | 1.38× |
| std_mutex | 5 | 1.90 ⚠ (1.86–2.04) | 1.26× |
| onetbb_cq_sp | 5 | 1.83 ⚠ (1.69–1.98) | 1.22× |
| boost_ms_queue | 5 | 1.78 (1.74–1.82) | 1.19× |
| shim_generic | 5 | 1.50 (1.42–1.53) | 1.00× |

## W1  E64  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic |
|---|---|---|---|
| shim_spsc | 5 | 3.56 ⚠ (3.36–3.65) | 3.26× |
| moodycamel_cq_sp | 5 | 2.55 (2.52–2.66) | 2.33× |
| onetbb_cq_sp | 5 | 1.87 ⚠ (1.83–2.12) | 1.71× |
| boost_ms_queue | 5 | 1.69 ⚠ (1.61–2.26) | 1.55× |
| std_mutex | 5 | 1.38 (1.30–1.39) | 1.26× |
| shim_generic | 5 | 1.09 (1.07–1.11) | 1.00× |

## W1  E8  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic |
|---|---|---|---|
| shim_spsc | 5 | 3.50 (3.47–3.56) | 3.08× |
| moodycamel_cq_sp | 5 | 3.02 ⚠ (2.72–3.07) | 2.66× |
| boost_ms_queue | 5 | 2.13 ⚠ (1.68–2.28) | 1.88× |
| onetbb_cq_sp | 5 | 1.92 ⚠ (1.54–2.09) | 1.69× |
| std_mutex | 5 | 1.35 ⚠ (1.30–1.41) | 1.19× |
| shim_generic | 5 | 1.13 ⚠ (1.09–1.20) | 1.00× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
