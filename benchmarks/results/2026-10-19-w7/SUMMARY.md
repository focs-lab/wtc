# w7  (2026-10-19T09:01:27Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-6, smt on, build `built by wtc-cc`, git e6581d1d0e4b (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 0.991, rep 2 1.004, rep 3 0.983, rep 4 1.000 (n=12) — no trend.  Widest single row: inline_plain E64, 29% across repetitions.

## W7  E64

| variant | n | Mops/s | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| inline_batch | 5 | 54.04 (52.61–54.40) | 2,075 | 3,814 | 6,514 | 34,037 |
| inline_plain | 5 | 16.62 ⚠ (15.37–19.87) | 456 | 1,998 | 16,525 | 36,136 |
| spsc_batch | 5 | 3.38 ⚠ (3.09–3.69) | 19,006 | 22,238 | 26,555 | 79,581 |
| spsc_plain | 5 | 3.25 ⚠ (2.99–3.37) | 883 | 2,266 | 4,634 | 71,682 |
| generic_batch | 5 | 1.91 ⚠ (1.77–2.11) | 34,125 | 40,002 | 92,922 | 184,151 |
| generic_plain | 5 | 0.99 (0.97–1.02) | 2,907 | 12,632 | 20,049 | 142,628 |

## W7  E8

| variant | n | Mops/s | p50 | p99 | p99.9 | max |
|---|---|---|---|---|---|---|
| inline_batch | 5 | 71.41 (71.32–71.44) | 0.00 | 0.00 | 0.00 | 0.00 |
| inline_plain | 5 | 70.01 (69.54–70.50) | 0.00 | 0.00 | 0.00 | 0.00 |
| spsc_batch | 5 | 3.48 ⚠ (3.18–3.79) | 0.00 | 0.00 | 0.00 | 0.00 |
| spsc_plain | 5 | 3.46 (3.31–3.55) | 0.00 | 0.00 | 0.00 | 0.00 |
| generic_batch | 5 | 1.74 ⚠ (1.65–1.96) | 0.00 | 0.00 | 0.00 | 0.00 |
| generic_plain | 5 | 1.03 ⚠ (0.97–1.12) | 0.00 | 0.00 | 0.00 | 0.00 |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
