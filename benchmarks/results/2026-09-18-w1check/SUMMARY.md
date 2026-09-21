# w1check  (2026-09-18T20:25:11Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=ON;`, git a778ce08b691 (dirty)

Control (shim_generic E64, median of 3 and 3): open 1.17, close 1.07 Mops/s, drift 8.2% — **NOISY: rerun this leg**

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.009, rep 2 1.042, rep 3 1.084, rep 4 1.009 (n=8) — no trend, but a wide spread (8.4%).  Widest single row: ring_inline E8, 47% across repetitions.

## W1  E64  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 65.34 ⚠ (59.87–74.92) | 57.72× | 1.00× |
| ring_sp | 5 | 3.42 ⚠ (3.27–3.65) | 3.02× | 0.05× |
| shim_spsc | 5 | 3.38 ⚠ (2.87–3.50) | 2.98× | 0.05× |
| shim_generic | 5 | 1.13 ⚠ (1.10–1.23) | 1.00× | 0.02× |

## W1  E8  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 19.35 ⚠ (14.98–22.02) | 17.11× | 1.00× |
| ring_sp | 5 | 3.69 ⚠ (3.37–3.98) | 3.27× | 0.19× |
| shim_spsc | 5 | 3.33 ⚠ (3.14–3.98) | 2.94× | 0.17× |
| shim_generic | 5 | 1.13 ⚠ (1.04–1.16) | 1.00× | 0.06× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
