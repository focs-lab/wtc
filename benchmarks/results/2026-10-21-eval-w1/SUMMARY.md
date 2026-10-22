# eval-w1  (2026-10-21T03:39:10Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=1;`, git 915a601a933b

Control (shim_generic E64, median of 3 and 3): open 1.04, close 1.08 Mops/s, drift 3.7% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.022, rep 2 1.029, rep 3 1.026, rep 4 1.020, rep 5 1.039, rep 6 1.016, rep 7 1.020, rep 8 1.020, rep 9 1.021 (n=48) — no trend.  Widest single row: rigtorp_inline in W1  E8  wait=spin, 299% across repetitions.

## W1  E256  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|---|
| shim_spsc_inline | 10 | 50.15 [49.75–50.33] | 19.94 | 32.98× | 1.05× |
| rigtorp_inline | 10 | 49.98 [49.59–50.98] | 20.01 | 32.87× | 1.05× |
| ring_inline | 10 | 47.64 [47.30–48.13] | 20.99 | 31.33× | 1.00× |
| ring_borrowed | 10 | 42.57 ⚠ [41.40–43.82] | 23.49 | 28.00× | 0.89× |
| moodycamel_rw_inline | 10 | 9.85 ⚠ [9.64–11.08] | 102 | 6.48× | 0.21× |
| rigtorp_sp | 10 | 3.51 [3.43–3.53] | 285 | 2.31× | 0.07× |
| shim_spsc | 10 | 3.39 ⚠ [3.23–3.48] | 295 | 2.23× | 0.07× |
| ring_sp | 10 | 3.20 ⚠ [3.00–3.37] | 312 | 2.11× | 0.07× |
| moodycamel_rw_sp | 10 | 3.08 ⚠ [2.91–3.21] | 324 | 2.03× | 0.06× |
| boost_spsc_sp | 10 | 2.37 [2.31–2.40] | 422 | 1.56× | 0.05× |
| std_mutex | 10 | 2.00 ⚠ [1.94–2.08] | 499 | 1.32× | 0.04× |
| moodycamel_cq_sp | 10 | 1.97 ⚠ [1.93–2.07] | 508 | 1.30× | 0.04× |
| onetbb_cq_sp | 10 | 1.82 ⚠ [1.73–1.88] | 550 | 1.20× | 0.04× |
| boost_ms_queue | 10 | 1.76 ⚠ [1.69–1.80] | 567 | 1.16× | 0.04× |
| orig_generic | 10 | 1.61 ⚠ [1.55–1.71] | 622 | 1.06× | 0.03× |
| shim_generic | 10 | 1.52 ⚠ [1.50–1.65] | 658 | 1.00× | 0.03× |

## W1  E64  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|---|
| ring_inline | 10 | 148 [146–149] | 6.77 | 133× | 1.00× |
| rigtorp_inline | 10 | 140 [138–141] | 7.17 | 126× | 0.95× |
| ring_borrowed | 10 | 127 [126–127] | 7.87 | 115× | 0.86× |
| shim_spsc_inline | 10 | 66.81 ⚠ [61.03–70.51] | 14.97 | 60.41× | 0.45× |
| moodycamel_rw_inline | 10 | 10.16 ⚠ [9.32–10.55] | 98.41 | 9.19× | 0.07× |
| rigtorp_sp | 10 | 3.67 ⚠ [3.60–3.83] | 272 | 3.32× | 0.02× |
| ring_sp | 10 | 3.66 ⚠ [3.44–3.86] | 273 | 3.31× | 0.02× |
| shim_spsc | 10 | 3.43 ⚠ [3.26–3.56] | 291 | 3.10× | 0.02× |
| moodycamel_rw_sp | 10 | 3.39 ⚠ [3.28–3.50] | 295 | 3.07× | 0.02× |
| boost_spsc_sp | 10 | 2.63 ⚠ [2.53–2.77] | 381 | 2.38× | 0.02× |
| moodycamel_cq_sp | 10 | 2.55 [2.49–2.55] | 393 | 2.30× | 0.02× |
| onetbb_cq_sp | 10 | 1.85 ⚠ [1.80–1.89] | 540 | 1.67× | 0.01× |
| boost_ms_queue | 10 | 1.83 ⚠ [1.75–2.12] | 548 | 1.65× | 0.01× |
| std_mutex | 10 | 1.31 ⚠ [1.22–1.39] | 761 | 1.19× | 1/112 |
| shim_generic | 10 | 1.11 ⚠ [1.07–1.15] | 904 | 1.00× | 1/133 |
| orig_generic | 10 | 1.06 ⚠ [1.03–1.11] | 946 | 0.96× | 1/140 |

## W1  E8  wait=spin

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|---|
| rigtorp_inline | 10 | 232 ⚠ [215–240] | 4.32 | 205× | 6.40× |
| ring_inline | 10 | 36.23 ⚠ [27.47–38.93] | 27.60 | 32.07× | 1.00× |
| ring_borrowed | 10 | 35.24 ⚠ [28.23–45.99] | 28.38 | 31.19× | 0.97× |
| shim_spsc_inline | 10 | 19.51 ⚠ [17.38–23.34] | 51.26 | 17.27× | 0.54× |
| moodycamel_rw_inline | 10 | 16.65 ⚠ [14.88–18.07] | 60.07 | 14.73× | 0.46× |
| shim_spsc | 10 | 3.76 ⚠ [3.43–3.88] | 266 | 3.32× | 0.10× |
| rigtorp_sp | 10 | 3.75 ⚠ [3.58–4.15] | 266 | 3.32× | 0.10× |
| ring_sp | 10 | 3.63 ⚠ [3.43–4.23] | 276 | 3.21× | 0.10× |
| moodycamel_rw_sp | 10 | 3.44 ⚠ [3.33–3.58] | 291 | 3.04× | 0.09× |
| moodycamel_cq_sp | 10 | 3.06 ⚠ [3.03–3.12] | 327 | 2.71× | 0.08× |
| boost_spsc_sp | 10 | 2.59 ⚠ [2.47–2.82] | 386 | 2.29× | 0.07× |
| onetbb_cq_sp | 10 | 1.91 ⚠ [1.78–2.00] | 524 | 1.69× | 0.05× |
| boost_ms_queue | 10 | 1.87 ⚠ [1.65–2.08] | 536 | 1.65× | 0.05× |
| std_mutex | 10 | 1.40 ⚠ [1.34–1.46] | 712 | 1.24× | 0.04× |
| shim_generic | 10 | 1.13 ⚠ [1.09–1.18] | 885 | 1.00× | 0.03× |
| orig_generic | 10 | 1.10 ⚠ [1.02–1.14] | 912 | 0.97× | 0.03× |

## W1  [mechanism pass]  E256  wait=spin

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | LLC read misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|---|
| boost_ms_queue | 3 | 1.02 | 1.00 | 882 | 701 | 0.01 | 0.00 | 0.79 | 12.03 |
| boost_spsc_sp | 3 | 1.00 | 1.00 | 766 | 707 | 0.01 | 0.00 | 0.92 | 6.02 |
| moodycamel_cq_sp | 3 | 1.02 | 1.02 | 1,046 | 860 | 0.00 | 0.00 | 0.82 | 5.16 |
| moodycamel_rw_inline | 3 | 0.00 | 0.00 | 393 | 85.50 | 0.00 | 0.00 | 0.22 | 3.44 |
| moodycamel_rw_sp | 3 | 1.00 | 1.00 | 545 | 652 | 0.00 | 0.00 | 1.20 | 5.16 |
| onetbb_cq_sp | 3 | 1.00 | 1.00 | 833 | 784 | 0.00 | 0.00 | 0.94 | 6.02 |
| orig_generic | 3 | 2.00 | 2.00 | 1,532 | 1,161 | 0.02 | 0.01 | 0.76 | 5.16 |
| rigtorp_inline | 3 | 0.00 | 0.00 | 83.62 | 61.01 | 0.01 | 0.00 | 0.73 | 18.91 |
| rigtorp_sp | 3 | 1.00 | 1.00 | 473 | 605 | 0.01 | 0.00 | 1.28 | 6.88 |
| ring_borrowed | 3 | 0.00 | 0.00 | 87.22 | 65.23 | 0.00 | 0.00 | 0.75 | 5.16 |
| ring_inline | 3 | 0.00 | 0.00 | 93.49 | 64.10 | 0.05 | 0.00 | 0.69 | 35.23 |
| ring_sp | 3 | 1.00 | 1.00 | 534 | 663 | 0.00 | 0.00 | 1.24 | 6.02 |
| shim_generic | 3 | 2.00 | 2.00 | 1,547 | 1,178 | 0.00 | 0.00 | 0.76 | 5.16 |
| shim_spsc | 3 | 1.00 | 1.00 | 504 | 673 | 0.01 | 0.00 | 1.34 | 6.02 |
| shim_spsc_inline | 3 | 0.00 | 0.00 | 77.05 | 132 | 0.00 | 0.00 | 1.72 | 5.16 |
| std_mutex | 3 | 1.03 | 1.03 | 797 | 839 | 0.01 | 0.00 | 1.05 | 4.30 |

## W1  [mechanism pass]  E64  wait=spin

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | LLC read misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|---|
| boost_ms_queue | 3 | 1.02 | 1.00 | 2,732 | 635 | 0.00 | 0.00 | 0.23 | 11.17 |
| boost_spsc_sp | 3 | 1.00 | 1.00 | 2,458 | 551 | 0.00 | 0.00 | 0.22 | 4.30 |
| moodycamel_cq_sp | 3 | 1.00 | 1.00 | 2,299 | 591 | 0.00 | 0.00 | 0.26 | 3.44 |
| moodycamel_rw_inline | 3 | 0.00 | 0.00 | 317 | 69.60 | 0.00 | 0.00 | 0.22 | 3.44 |
| moodycamel_rw_sp | 3 | 1.00 | 1.00 | 1,899 | 505 | 0.00 | 0.00 | 0.27 | 3.44 |
| onetbb_cq_sp | 3 | 1.00 | 1.00 | 2,755 | 662 | 0.00 | 0.00 | 0.24 | 3.44 |
| orig_generic | 3 | 2.00 | 2.00 | 3,917 | 1,202 | 0.00 | 0.00 | 0.31 | 3.44 |
| rigtorp_inline | 3 | 0.00 | 0.00 | 31.33 | 48.32 | 0.00 | 0.00 | 1.54 | 7.73 |
| rigtorp_sp | 3 | 1.00 | 1.00 | 1,886 | 399 | 0.00 | 0.00 | 0.21 | 4.30 |
| ring_borrowed | 3 | 0.00 | 0.00 | 29.74 | 53.10 | 0.00 | 0.00 | 1.79 | 3.44 |
| ring_inline | 3 | 0.00 | 0.00 | 34.52 | 50.71 | 0.00 | 0.00 | 1.47 | 11.17 |
| ring_sp | 3 | 1.00 | 1.00 | 1,732 | 497 | 0.00 | 0.00 | 0.29 | 3.44 |
| shim_generic | 3 | 2.00 | 2.00 | 3,105 | 1,076 | 0.00 | 0.00 | 0.35 | 3.44 |
| shim_spsc | 3 | 1.00 | 1.00 | 1,800 | 544 | 0.00 | 0.00 | 0.30 | 3.44 |
| shim_spsc_inline | 3 | 0.00 | 0.00 | 84.01 | 122 | 0.00 | 0.00 | 1.46 | 3.44 |
| std_mutex | 3 | 1.03 | 1.03 | 1,794 | 672 | 0.00 | 0.00 | 0.37 | 3.44 |

## W1  [mechanism pass]  E8  wait=spin

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | LLC read misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|---|
| boost_ms_queue | 3 | 1.02 | 1.00 | 2,941 | 682 | 0.00 | 0.00 | 0.23 | 11.17 |
| boost_spsc_sp | 3 | 1.00 | 1.00 | 2,365 | 534 | 0.00 | 0.00 | 0.23 | 4.30 |
| moodycamel_cq_sp | 3 | 1.00 | 1.00 | 2,329 | 615 | 0.00 | 0.00 | 0.26 | 3.44 |
| moodycamel_rw_inline | 3 | 0.00 | 0.00 | 167 | 57.69 | 0.00 | 0.00 | 0.35 | 3.44 |
| moodycamel_rw_sp | 3 | 1.00 | 1.00 | 2,052 | 534 | 0.00 | 0.00 | 0.26 | 3.44 |
| onetbb_cq_sp | 3 | 1.00 | 1.00 | 2,886 | 661 | 0.00 | 0.00 | 0.23 | 3.44 |
| orig_generic | 3 | 2.00 | 2.00 | 3,270 | 1,141 | 0.00 | 0.00 | 0.35 | 3.44 |
| rigtorp_inline | 3 | 0.00 | 0.00 | 21.79 | 43.41 | 0.00 | 0.00 | 1.99 | 3.44 |
| rigtorp_sp | 3 | 1.00 | 1.00 | 1,827 | 370 | 0.00 | 0.00 | 0.20 | 4.30 |
| ring_borrowed | 3 | 0.00 | 0.00 | 97.36 | 48.96 | 0.00 | 0.00 | 0.50 | 3.44 |
| ring_inline | 3 | 0.00 | 0.00 | 136 | 46.19 | 0.00 | 0.00 | 0.34 | 4.30 |
| ring_sp | 3 | 1.00 | 1.00 | 1,872 | 494 | 0.00 | 0.00 | 0.26 | 3.44 |
| shim_generic | 3 | 2.00 | 2.00 | 2,955 | 1,126 | 0.00 | 0.00 | 0.38 | 3.44 |
| shim_spsc | 3 | 1.00 | 1.00 | 1,715 | 547 | 0.00 | 0.00 | 0.32 | 3.44 |
| shim_spsc_inline | 3 | 0.00 | 0.00 | 155 | 118 | 0.00 | 0.00 | 0.76 | 3.44 |
| std_mutex | 3 | 1.03 | 1.03 | 1,656 | 682 | 0.00 | 0.00 | 0.41 | 3.44 |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
