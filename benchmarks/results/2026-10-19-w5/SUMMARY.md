# w5  (2026-10-19T19:26:06Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-7, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git e363247b7845 (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 0.848, rep 2 0.993, rep 3 0.885, rep 4 0.928 (n=6) — no trend, but a wide spread (15.2%).  Widest single row: orig_generic E64, 44% across repetitions.

## W5  E64  wait=spin

| variant | n | Mops/s | p50 | p99 | p99.9 | max | Mops/s vs shim_generic | p99 vs shim_generic | p99.9 vs shim_generic |
|---|---|---|---|---|---|---|---|---|---|
| onetbb_cq_sp | 5 | 3.11 ⚠ (2.55–3.21) | 2,776,780 | 2,859,880 | 2,895,940 | 2,917,790 | 3.35× | 0.29× | 0.29× |
| moodycamel_cq_sp | 5 | 2.22 ⚠ (2.13–2.49) | 4,042,870 | 4,344,830 | 4,390,810 | 4,620,760 | 2.39× | 0.44× | 0.44× |
| boost_ms_queue | 5 | 1.98 ⚠ (1.65–2.20) | 1,562 | 3,457 | 8,057 | 539,684 | 2.13× | 1/2,879 | 1/1,242 |
| shim_generic | 5 | 0.93 ⚠ (0.85–1.06) | 9,578,980 | 9,954,450 | 10,003,200 | 10,244,500 | 1.00× | 1.00× | 1.00× |
| std_mutex | 5 | 0.89 ⚠ (0.88–1.06) | 30,322 | 273,402 | 388,867 | 512,378 | 0.96× | 0.03× | 0.04× |
| orig_generic | 5 | 0.69 ⚠ (0.59–0.89) | 12,819,900 | 13,362,100 | 13,498,300 | 13,721,300 | 0.75× | 1.34× | 1.35× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
