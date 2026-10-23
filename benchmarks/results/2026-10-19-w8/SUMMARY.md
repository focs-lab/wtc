# w8  (2026-10-19T19:29:56Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-7, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git e363247b7845 (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.006, rep 2 1.000, rep 3 1.002, rep 4 1.000 (n=8) — no trend.  Widest single row: plain threads=2 work=0, 40% across repetitions.

## W8  threads=1  work=0

| variant | n | Mops/s | finish spread | Mops/s vs plain |
|---|---|---|---|---|
| batched | 5 | 599 (587–616) | 0.0% (max 0.0%) | 16.98× |
| plain | 5 | 35.24 (35.21–35.38) | 0.0% (max 0.0%) | 1.00× |

## W8  threads=1  work=64

| variant | n | Mops/s | finish spread | Mops/s vs plain |
|---|---|---|---|---|
| batched | 5 | 58.83 (58.82–58.86) | 0.0% (max 0.0%) | 1.76× |
| plain | 5 | 33.38 (33.36–33.39) | 0.0% (max 0.0%) | 1.00× |

## W8  threads=2  work=0

| variant | n | Mops/s | finish spread | Mops/s vs plain |
|---|---|---|---|---|
| batched | 5 | 201 ⚠ (164–223) | 0.6% (max 2.4%) | 28.36× |
| plain | 5 | 7.08 ⚠ (6.60–9.40) | 0.5% (max 1.7%) | 1.00× |

## W8  threads=2  work=64

| variant | n | Mops/s | finish spread | Mops/s vs plain |
|---|---|---|---|---|
| batched | 5 | 41.80 (40.80–42.42) | 2.0% (max 3.6%) | 5.46× |
| plain | 5 | 7.65 ⚠ (6.52–7.86) | 0.6% (max 1.2%) | 1.00× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
