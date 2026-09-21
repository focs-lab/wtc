# smt  (2026-09-18T20:41:28Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5,60,61, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;`, git a778ce08b691 (dirty)

Control (shim_generic E64, median of 3 and 3): open 1.03, close 1.02 Mops/s, drift 0.6% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.003, rep 2 1.000, rep 3 1.001, rep 4 1.004 (n=8) — no trend.

## W1  E64  wait=spin  (SMT siblings)

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 141 (141–141) | 45.97× | 1.00× |
| ring_sp | 5 | 12.71 (12.69–12.80) | 4.14× | 0.09× |
| shim_spsc | 5 | 12.66 (12.64–12.66) | 4.12× | 0.09× |
| shim_generic | 5 | 3.07 (3.06–3.10) | 1.00× | 0.02× |
| orig_generic | 5 | 3.06 (3.05–3.09) | 1.00× | 0.02× |

## W1  E64  wait=yield  (SMT siblings)

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 5 | 137 (137–138) | 44.83× | 1.00× |
| shim_spsc | 5 | 12.89 (12.84–12.92) | 4.22× | 0.09× |
| shim_generic | 5 | 3.06 (3.02–3.07) | 1.00× | 0.02× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
