# w5  (2026-09-18T19:22:32Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-7, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;`, git a5a1c2a6b734 (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 0.891, rep 2 0.921, rep 3 1.053, rep 4 0.970 (n=5) — no trend, but a wide spread (16.2%).  Widest single row: std_mutex E64, 25% across repetitions.

## W5  E64  wait=spin

| variant | n | Mops/s | p50 | p99 | p99.9 | max | Mops/s vs shim_generic | p99 vs shim_generic | p99.9 vs shim_generic |
|---|---|---|---|---|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 2.21 ⚠ (2.04–2.47) | 4,055,830 | 4,347,710 | 4,375,660 | 4,825,890 | 2.36× | 0.44× | 0.44× |
| boost_ms_queue | 5 | 1.86 ⚠ (1.66–2.01) | 1,617 | 3,514 | 5,806 | 483,068 | 1.98× | 1/2,805 | 1/1,711 |
| std_mutex | 5 | 0.99 ⚠ (0.94–1.19) | 8,722,250 | 8,914,950 | 8,989,800 | 9,160,310 | 1.06× | 0.90× | 0.90× |
| shim_generic | 5 | 0.94 ⚠ (0.87–1.05) | 9,458,850 | 9,855,200 | 9,935,050 | 10,036,100 | 1.00× | 1.00× | 1.00× |
| orig_generic | 5 | 0.88 ⚠ (0.85–0.99) | 10,083,100 | 10,416,800 | 10,521,700 | 10,721,700 | 0.94× | 1.06× | 1.06× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
