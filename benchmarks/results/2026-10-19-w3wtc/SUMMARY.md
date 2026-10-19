# w3wtc  (2026-10-19T08:57:27Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-6, smt on, build `built by wtc-cc`, git e6581d1d0e4b (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.022, rep 2 1.000, rep 3 1.000, rep 4 0.999 (n=8) — no trend.  Widest single row: hand E64, 17% across repetitions.

## W3wtc closed  E64

| variant | n | Mops/s | p50 | p99 | p99.9 | max | Mops/s vs generic | p99 vs generic | p99.9 vs generic |
|---|---|---|---|---|---|---|---|---|---|
| hand-inline | 5 | 8.40 (8.21–8.66) | 309,417,000 | 555,342,000 | 559,918,000 | 560,419,000 | 9.20× | 0.10× | 0.10× |
| compiler | 5 | 7.70 ⚠ (7.40–8.68) | 360,590,000 | 646,908,000 | 652,203,000 | 652,791,000 | 8.43× | 0.12× | 0.12× |
| hand | 5 | 3.18 ⚠ (2.99–3.54) | 3,384 | 14,732 | 434,954 | 573,440 | 3.48× | 1/374,283 | 1/12,744 |
| generic | 5 | 0.91 ⚠ (0.90–0.97) | 3,936,510,000 | 5,513,970,000 | 5,542,920,000 | 5,546,140,000 | 1.00× | 1.00× | 1.00× |

## W3wtc open  E64

| variant | n | Mops/s | p50 | p99 | p99.9 | max | Mops/s vs generic | p99 vs generic | p99.9 vs generic |
|---|---|---|---|---|---|---|---|---|---|
| hand | 5 | 2.00 (2.00–2.00) | 1,325 | 2,818 | 18,654 | 294,959 | 1.99× | 1/1,359,747 | 1/206,336 |
| hand-inline | 5 | 2.00 (2.00–2.00) | 600 | 1,150 | 5,187 | 291,767 | 1.99× | 1/3,330,465 | 1/741,992 |
| compiler | 5 | 2.00 (2.00–2.00) | 595 | 1,185 | 5,047 | 288,897 | 1.99× | 1/3,232,641 | 1/762,619 |
| generic | 5 | 1.01 ⚠ (0.89–1.02) | 2,985,320,000 | 3,831,100,000 | 3,848,890,000 | 3,850,880,000 | 1.00× | 1.00× | 1.00× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
