# w3wtc  (2026-09-18T19:08:59Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-7, smt on, build ``, git 8cf5c44ee813 (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.049, rep 2 1.033, rep 3 1.029, rep 4 1.031 (n=6) — no trend.  Widest single row: compiler E64, 21% across repetitions.

## W3wtc closed  E64

| variant | n | Mops/s | p50 | p99 | p99.9 | max | Mops/s vs generic | p99 vs generic | p99.9 vs generic |
|---|---|---|---|---|---|---|---|---|---|
| compiler | 5 | 3.07 ⚠ (2.79–3.37) | 2,847 | 9,540 | 399,802 | 560,843 | 3.07× | 1/449,998 | 1/10,780 |
| hand | 5 | 2.93 ⚠ (2.59–3.07) | 2,992 | 11,077 | 411,214 | 559,384 | 2.93× | 1/387,563 | 1/10,480 |
| generic | 5 | 1.00 (0.96–1.02) | 3,282,280,000 | 4,293,030,000 | 4,309,710,000 | 4,311,260,000 | 1.00× | 1.00× | 1.00× |

## W3wtc open  E64

| variant | n | Mops/s | p50 | p99 | p99.9 | max | Mops/s vs generic | p99 vs generic | p99.9 vs generic |
|---|---|---|---|---|---|---|---|---|---|
| hand | 5 | 2.00 (2.00–2.00) | 1,437 | 3,024 | 12,572 | 273,628 | 2.12× | 1/1,367,080 | 1/330,033 |
| compiler | 5 | 2.00 (2.00–2.00) | 1,738 | 3,382 | 20,151 | 273,056 | 2.12× | 1/1,222,337 | 1/205,910 |
| generic | 5 | 0.95 ⚠ (0.90–1.00) | 3,272,130,000 | 4,134,420,000 | 4,149,270,000 | 4,150,710,000 | 1.00× | 1.00× | 1.00× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
