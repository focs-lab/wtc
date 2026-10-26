# eval-fusion  (2026-10-26T10:09:55Z)

AMD EPYC 9115 16-Core Processor, kernel 6.8.0-137-generic, allowed cpus 16-31,48-63, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=0;`, git 

Control (shim_generic E64, median of 3 and 3): open 15.57, close 15.65 Mops/s, drift 0.5% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 0.999, rep 2 0.999, rep 3 1.000, rep 4 0.999 (n=18) — no trend.

## W12  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| fused | 5 | 2,827 [2,752–2,829] | 0.35 | 1.00× |
| pipeline | 5 | 84.75 [83.90–86.76] | 11.80 | 0.03× |

## W12  work=10  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| fused | 5 | 88.62 [87.60–89.22] | 11.28 | 1.00× |
| pipeline | 5 | 51.11 [50.62–51.50] | 19.56 | 0.58× |

## W12  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| pipeline | 5 | 9.19 [9.19–9.20] | 109 | 2.45× |
| fused | 5 | 3.75 [3.74–3.75] | 267 | 1.00× |

## W12  work=2  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| fused | 5 | 577 [576–578] | 1.73 | 1.00× |
| pipeline | 5 | 72.45 [71.44–73.79] | 13.80 | 0.13× |

## W12  work=20  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| pipeline | 5 | 34.17 [33.92–34.23] | 29.26 | 1.12× |
| fused | 5 | 30.47 [29.61–30.52] | 32.81 | 1.00× |

## W12  work=200  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| pipeline | 5 | 4.78 [4.77–4.79] | 209 | 2.73× |
| fused | 5 | 1.75 [1.74–1.76] | 570 | 1.00× |

## W12  work=40  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| pipeline | 5 | 20.57 [20.56–20.58] | 48.62 | 1.77× |
| fused | 5 | 11.60 [11.59–11.60] | 86.23 | 1.00× |

## W12  work=5  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| fused | 5 | 229 [228–229] | 4.37 | 1.00× |
| pipeline | 5 | 61.68 [61.52–62.42] | 16.21 | 0.27× |

## W12  work=60  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs fused |
|---|---|---|---|---|
| pipeline | 5 | 14.49 [14.49–14.50] | 69.00 | 2.14× |
| fused | 5 | 6.78 [6.77–6.79] | 147 | 1.00× |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
