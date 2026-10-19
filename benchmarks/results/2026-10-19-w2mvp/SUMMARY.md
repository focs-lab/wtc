# w2mvp  (2026-10-19T19:21:04Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-7, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git e363247b7845 (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 0.906, rep 2 0.999, rep 3 1.009, rep 4 0.821 (n=8) — no trend, but a wide spread (18.8%).  Widest single row: shim_generic E64 @1000, 77% across repetitions.

## W2 open  E64  wait=spin  interval=1000 ns

| variant | n | p50 | p90 | p99 | p99.9 | p99.99 | max | p99 vs shim_generic | p99.9 vs shim_generic |
|---|---|---|---|---|---|---|---|---|---|
| ring_borrowed | 5 | 195 | 300 | 323 ⚠ (279–330) | 1,637 | 39,896 | 121,268 | 0.03× | 0.04× |
| shim_spsc_inline | 5 | 225 | 309 | 336 ⚠ (295–391) | 1,598 | 46,432 | 132,786 | 0.03× | 0.04× |
| shim_spsc | 5 | 691 | 1,624 | 1,908 ⚠ (1,687–1,990) | 3,656 | 54,437 | 135,532 | 0.16× | 0.08× |
| shim_generic | 5 | 3,076 | 7,125 | 11,698 ⚠ (6,326–44,106) | 45,609 | 128,345 | 147,551 | 1.00× | 1.00× |

## W2 open  E64  wait=spin  interval=10000 ns

| variant | n | p50 | p90 | p99 | p99.9 | p99.99 | max | p99 vs shim_generic | p99.9 vs shim_generic |
|---|---|---|---|---|---|---|---|---|---|
| ring_borrowed | 5 | 236 | 311 | 325 ⚠ (283–373) | 507 | 4,958 | 129,211 | 0.07× | 0.07× |
| shim_spsc_inline | 5 | 276 | 322 | 333 (328–338) | 499 | 3,833 | 129,307 | 0.07× | 0.07× |
| shim_spsc | 5 | 658 | 1,374 | 1,626 ⚠ (1,459–1,921) | 1,946 | 4,882 | 124,229 | 0.36× | 0.29× |
| shim_generic | 5 | 1,668 | 2,556 | 4,511 ⚠ (3,436–6,482) | 6,813 | 11,470 | 128,013 | 1.00× | 1.00× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
