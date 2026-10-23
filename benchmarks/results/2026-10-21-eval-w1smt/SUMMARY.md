# eval-w1smt  (2026-10-21T14:29:28Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,60, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git fae9abeddedd

Control (shim_generic E64, median of 3 and 3): open 3.10, close 3.06 Mops/s, drift 1.3% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.001, rep 2 0.995, rep 3 0.997, rep 4 0.993, rep 5 0.996, rep 6 0.996, rep 7 0.995, rep 8 0.994, rep 9 0.992 (n=6) — no trend.

## W1  E64  wait=spin  (smt)

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic | vs rigtorp_inline |
|---|---|---|---|---|---|
| rigtorp_inline | 10 | 153 [152–153] | 6.53 | 49.84× | 1.00× |
| shim_spsc_inline | 10 | 67.02 [66.64–67.26] | 14.92 | 21.83× | 0.44× |
| shim_spsc | 10 | 13.34 [13.30–13.36] | 74.94 | 4.35× | 0.09× |
| moodycamel_rw_sp | 10 | 12.91 [12.89–12.96] | 77.47 | 4.20× | 0.08× |
| onetbb_cq_sp | 10 | 9.93 [9.91–9.97] | 101 | 3.23× | 0.06× |
| shim_generic | 10 | 3.07 [3.04–3.09] | 326 | 1.00× | 0.02× |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
