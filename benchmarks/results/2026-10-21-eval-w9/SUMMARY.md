# eval-w9  (2026-10-21T22:32:18Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-19, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git 63c2ba7213ad

Control (shim_generic E64, median of 3 and 3): open 1.06, close 1.06 Mops/s, drift 0.6% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.001, rep 2 1.001, rep 3 0.997, rep 4 0.995, rep 5 1.005, rep 6 1.001, rep 7 1.013, rep 8 1.003, rep 9 1.004 (n=98) — no trend.  Widest single row: onetbb_cq_sp in W9 pairwise  E8  threads=4  wait=spin, 46% across repetitions.

## W9 pairwise  E8  threads=1  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| onetbb_cq_sp | 10 | 28.71 [28.33–28.85] | 34.83 | 2.34× |
| std_mutex | 10 | 25.87 [25.86–25.88] | 38.66 | 2.11× |
| boost_ms_queue | 10 | 24.67 [24.66–24.70] | 40.54 | 2.01× |
| moodycamel_cq_sp | 10 | 23.84 [23.53–23.85] | 41.95 | 1.94× |
| shim_generic | 10 | 12.26 [12.22–12.27] | 81.54 | 1.00× |
| orig_generic | 10 | 12.13 ⚠ [12.09–12.13] | 82.45 | 0.99× |

## W9 pairwise  E8  threads=16  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 9.81 [9.55–10.03] | 102 | 4.49× |
| onetbb_cq_sp | 10 | 4.18 ⚠ [3.86–4.38] | 239 | 1.92× |
| boost_ms_queue | 10 | 3.15 ⚠ [3.01–3.28] | 317 | 1.44× |
| std_mutex | 10 | 2.20 ⚠ [2.06–2.39] | 455 | 1.01× |
| shim_generic | 10 | 2.18 ⚠ [2.11–2.27] | 458 | 1.00× |
| orig_generic | 10 | 1.98 ⚠ [1.88–2.08] | 505 | 0.91× |

## W9 pairwise  E8  threads=2  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 7.58 ⚠ [6.87–8.50] | 132 | 1.53× |
| onetbb_cq_sp | 10 | 6.32 ⚠ [5.90–7.25] | 158 | 1.28× |
| orig_generic | 10 | 4.99 [4.88–5.12] | 200 | 1.01× |
| shim_generic | 10 | 4.95 [4.87–5.06] | 202 | 1.00× |
| boost_ms_queue | 10 | 4.15 ⚠ [3.75–4.70] | 241 | 0.84× |
| std_mutex | 10 | 3.41 ⚠ [2.84–3.64] | 293 | 0.69× |

## W9 pairwise  E8  threads=4  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 7.17 ⚠ [6.65–7.58] | 139 | 2.57× |
| onetbb_cq_sp | 10 | 7.10 ⚠ [6.42–8.36] | 141 | 2.55× |
| boost_ms_queue | 10 | 3.93 ⚠ [3.82–4.03] | 255 | 1.41× |
| std_mutex | 10 | 3.47 ⚠ [3.36–3.64] | 288 | 1.25× |
| shim_generic | 10 | 2.79 ⚠ [2.67–2.85] | 359 | 1.00× |
| orig_generic | 10 | 2.70 ⚠ [2.59–2.79] | 371 | 0.97× |

## W9 pairwise  E8  threads=8  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 8.57 ⚠ [8.21–8.65] | 117 | 3.78× |
| onetbb_cq_sp | 10 | 6.15 ⚠ [6.06–7.07] | 162 | 2.72× |
| boost_ms_queue | 10 | 3.46 ⚠ [3.26–3.86] | 289 | 1.53× |
| shim_generic | 10 | 2.26 ⚠ [2.22–2.51] | 442 | 1.00× |
| std_mutex | 10 | 2.19 ⚠ [1.96–2.44] | 457 | 0.97× |
| orig_generic | 10 | 2.17 ⚠ [2.08–2.36] | 461 | 0.96× |

## W9 split  E8  producers=1  consumers=1  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 10 | 3.65 ⚠ [3.46–3.85] | 274 | 3.61× |
| moodycamel_cq_sp | 10 | 2.53 [2.51–2.56] | 395 | 2.50× |
| shim_mpsc | 10 | 1.92 ⚠ [1.88–1.99] | 522 | 1.89× |
| onetbb_cq_sp | 10 | 1.71 ⚠ [1.65–1.91] | 586 | 1.69× |
| boost_ms_queue | 10 | 1.49 ⚠ [1.42–1.55] | 669 | 1.47× |
| std_mutex | 10 | 1.23 ⚠ [1.19–1.31] | 810 | 1.22× |
| orig_generic | 10 | 1.05 ⚠ [1.03–1.11] | 955 | 1.03× |
| shim_generic | 10 | 1.01 ⚠ [0.97–1.06] | 987 | 1.00× |

## W9 split  E8  producers=1  consumers=2  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 10 | 3.18 ⚠ [2.95–3.42] | 315 | 2.92× |
| moodycamel_cq_sp | 10 | 2.37 ⚠ [2.20–2.53] | 422 | 2.18× |
| onetbb_cq_sp | 10 | 1.43 ⚠ [1.41–1.55] | 697 | 1.32× |
| boost_ms_queue | 10 | 1.31 ⚠ [1.22–1.37] | 763 | 1.20× |
| orig_generic | 10 | 1.11 ⚠ [1.07–1.15] | 899 | 1.02× |
| shim_generic | 10 | 1.09 ⚠ [1.05–1.16] | 918 | 1.00× |
| std_mutex | 10 | 0.80 ⚠ [0.76–0.86] | 1,255 | 0.73× |

## W9 split  E8  producers=1  consumers=4  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 2.24 ⚠ [2.19–2.42] | 447 | 2.29× |
| shim_spmc | 10 | 2.16 ⚠ [1.99–2.24] | 462 | 2.21× |
| onetbb_cq_sp | 10 | 1.49 ⚠ [1.43–1.55] | 671 | 1.52× |
| boost_ms_queue | 10 | 1.25 ⚠ [1.14–1.32] | 799 | 1.28× |
| orig_generic | 10 | 0.99 ⚠ [0.98–1.02] | 1,005 | 1.02× |
| shim_generic | 10 | 0.98 ⚠ [0.96–0.99] | 1,021 | 1.00× |
| std_mutex | 10 | 0.46 ⚠ [0.43–0.48] | 2,153 | 0.47× |

## W9 split  E8  producers=1  consumers=8  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 10 | 1.71 ⚠ [1.59–1.74] | 584 | 1.99× |
| moodycamel_cq_sp | 10 | 1.69 ⚠ [1.58–1.76] | 591 | 1.96× |
| onetbb_cq_sp | 10 | 1.40 ⚠ [1.36–1.46] | 716 | 1.62× |
| orig_generic | 10 | 0.94 ⚠ [0.85–1.06] | 1,064 | 1.09× |
| boost_ms_queue | 10 | 0.92 ⚠ [0.87–0.96] | 1,082 | 1.07× |
| shim_generic | 10 | 0.86 ⚠ [0.83–0.93] | 1,160 | 1.00× |
| std_mutex | 10 | 0.27 ⚠ [0.25–0.28] | 3,715 | 0.31× |

## W9 split  E8  producers=2  consumers=1  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 3.60 [3.57–3.61] | 278 | 2.86× |
| onetbb_cq_sp | 10 | 3.45 ⚠ [3.33–3.71] | 290 | 2.75× |
| shim_mpsc | 10 | 3.10 [3.04–3.14] | 322 | 2.47× |
| boost_ms_queue | 10 | 2.14 ⚠ [2.07–2.18] | 467 | 1.71× |
| std_mutex | 10 | 1.66 ⚠ [1.63–1.75] | 602 | 1.32× |
| orig_generic | 10 | 1.37 ⚠ [1.33–1.39] | 729 | 1.09× |
| shim_generic | 10 | 1.26 ⚠ [1.23–1.33] | 797 | 1.00× |

## W9 split  E8  producers=2  consumers=2  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| onetbb_cq_sp | 10 | 2.93 ⚠ [2.87–2.99] | 341 | 2.83× |
| moodycamel_cq_sp | 10 | 2.83 ⚠ [2.64–2.90] | 353 | 2.73× |
| boost_ms_queue | 10 | 1.78 ⚠ [1.66–1.99] | 562 | 1.72× |
| std_mutex | 10 | 1.14 ⚠ [1.07–1.25] | 874 | 1.11× |
| orig_generic | 10 | 1.05 ⚠ [0.96–1.06] | 951 | 1.02× |
| shim_generic | 10 | 1.03 ⚠ [0.99–1.06] | 966 | 1.00× |

## W9 split  E8  producers=4  consumers=1  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 5.38 [5.37–5.41] | 186 | 5.12× |
| onetbb_cq_sp | 10 | 3.89 ⚠ [3.76–4.07] | 257 | 3.69× |
| shim_mpsc | 10 | 3.39 [3.31–3.42] | 295 | 3.22× |
| boost_ms_queue | 10 | 2.29 ⚠ [2.04–2.57] | 436 | 2.18× |
| std_mutex | 10 | 1.47 ⚠ [1.37–1.56] | 682 | 1.39× |
| orig_generic | 10 | 1.17 ⚠ [1.09–1.20] | 854 | 1.11× |
| shim_generic | 10 | 1.05 ⚠ [0.98–1.11] | 950 | 1.00× |

## W9 split  E8  producers=4  consumers=4  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| onetbb_cq_sp | 10 | 4.76 ⚠ [3.83–5.38] | 210 | 5.79× |
| moodycamel_cq_sp | 10 | 3.95 ⚠ [3.81–4.05] | 253 | 4.80× |
| boost_ms_queue | 10 | 1.88 ⚠ [1.82–2.06] | 532 | 2.28× |
| std_mutex | 10 | 0.91 ⚠ [0.78–0.93] | 1,104 | 1.10× |
| orig_generic | 10 | 0.83 ⚠ [0.76–0.96] | 1,200 | 1.01× |
| shim_generic | 10 | 0.82 ⚠ [0.74–0.90] | 1,216 | 1.00× |

## W9 split  E8  producers=8  consumers=1  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 5.49 [5.48–5.52] | 182 | 5.33× |
| onetbb_cq_sp | 10 | 4.37 ⚠ [4.24–4.62] | 229 | 4.24× |
| shim_mpsc | 10 | 4.10 [4.07–4.12] | 244 | 3.98× |
| boost_ms_queue | 10 | 1.91 ⚠ [1.72–1.98] | 524 | 1.85× |
| std_mutex | 10 | 1.44 ⚠ [1.34–1.62] | 697 | 1.39× |
| orig_generic | 10 | 1.06 ⚠ [0.97–1.16] | 940 | 1.03× |
| shim_generic | 10 | 1.03 ⚠ [0.95–1.09] | 970 | 1.00× |

## W9 split  E8  producers=8  consumers=8  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 10 | 5.12 [5.00–5.17] | 195 | 7.74× |
| onetbb_cq_sp | 10 | 4.85 ⚠ [4.37–5.15] | 206 | 7.33× |
| boost_ms_queue | 10 | 1.41 ⚠ [1.39–1.57] | 711 | 2.13× |
| std_mutex | 10 | 0.84 ⚠ [0.80–0.90] | 1,193 | 1.27× |
| orig_generic | 10 | 0.78 ⚠ [0.67–0.79] | 1,278 | 1.18× |
| shim_generic | 10 | 0.66 ⚠ [0.64–0.76] | 1,511 | 1.00× |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
