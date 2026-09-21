# w5  (2026-09-19T05:28:36Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-7, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git 5d5454c69159 (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.064, rep 2 0.967, rep 3 1.030, rep 4 1.080 (n=6) — no trend, but a wide spread (11.3%).  Widest single row: std_mutex E64, 20% across repetitions.

## W5  E64  wait=spin

| variant | n | Mops/s | p50 | p99 | p99.9 | max | Mops/s vs shim_generic | p99 vs shim_generic | p99.9 vs shim_generic |
|---|---|---|---|---|---|---|---|---|---|
| onetbb_cq_sp | 5 | 2.96 ⚠ (2.71–3.07) | 3,042,680 | 3,100,260 | 3,158,640 | 3,245,620 | 3.11× | 0.32× | 0.32× |
| moodycamel_cq_sp | 5 | 2.22 ⚠ (2.15–2.49) | 4,044,390 | 4,335,780 | 4,369,440 | 4,716,300 | 2.33× | 0.45× | 0.44× |
| boost_ms_queue | 5 | 1.67 ⚠ (1.61–1.86) | 1,877 | 3,843 | 5,215 | 373,751 | 1.76× | 1/2,523 | 1/1,921 |
| std_mutex | 5 | 1.04 ⚠ (0.93–1.13) | 8,400,720 | 8,555,900 | 9,036,510 | 9,242,320 | 1.10× | 0.88× | 0.90× |
| orig_generic | 5 | 0.96 ⚠ (0.94–1.03) | 9,281,010 | 9,476,770 | 9,523,110 | 9,719,360 | 1.01× | 0.98× | 0.95× |
| shim_generic | 5 | 0.95 ⚠ (0.89–1.07) | 9,285,180 | 9,695,710 | 10,019,200 | 10,342,500 | 1.00× | 1.00× | 1.00× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
