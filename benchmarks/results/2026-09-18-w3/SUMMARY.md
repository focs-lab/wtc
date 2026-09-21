# w3  (2026-09-18T19:14:26Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-7, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;`, git 8cf5c44ee813 (dirty)

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.003, rep 2 1.006, rep 3 1.004, rep 4 1.025 (n=14) — no trend.  Widest single row: moodycamel_rw_sp E64, 40% across repetitions.

## W3 closed  E64  wait=spin

| variant | n | Mops/s | p50 | p90 | p99 | p99.9 | p99.99 | max | Mops/s vs shim_generic | p99 vs shim_generic | p99.9 vs shim_generic | vs ring_inline |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ring_inline | 5 | 9.84 ⚠ (7.31–9.90) | 447,188 | 471,614 | 481,512 | 598,320 | 713,289 | 730,963 | 9.11× | 0.12× | 0.15× | 1.00× |
| moodycamel_rw_sp | 5 | 3.06 ⚠ (2.38–3.35) | 5,148 | 10,109 | 16,407 | 379,204 | 462,160 | 488,743 | 2.84× | 1/244 | 0.09× | 0.31× |
| ring_sp | 5 | 3.05 ⚠ (2.57–3.11) | 6,069 | 11,883 | 17,801 | 458,993 | 536,755 | 560,206 | 2.82× | 1/225 | 0.11× | 0.31× |
| shim_spsc | 5 | 3.04 ⚠ (2.65–3.32) | 3,243 | 5,965 | 10,223 | 408,720 | 519,370 | 561,203 | 2.81× | 1/391 | 0.10× | 0.31× |
| rigtorp_sp | 5 | 2.80 ⚠ (2.71–3.39) | 14,177 | 22,479 | 94,789 | 538,827 | 603,242 | 629,073 | 2.59× | 0.02× | 0.13× | 0.28× |
| shim_generic | 5 | 1.08 ⚠ (1.05–1.13) | 3,888,430 | 3,960,880 | 4,002,050 | 4,032,720 | 4,309,530 | 4,410,810 | 1.00× | 1.00× | 1.00× | 0.11× |
| orig_generic | 5 | 1.03 ⚠ (0.81–1.16) | 4,179,110 | 4,290,660 | 4,339,680 | 4,381,200 | 4,989,260 | 5,046,590 | 0.95× | 1.08× | 1.09× | 0.10× |

## W3 open  E64  wait=spin

| variant | n | Mops/s | p50 | p90 | p99 | p99.9 | p99.99 | max | Mops/s vs shim_generic | p99 vs shim_generic | p99.9 vs shim_generic | vs ring_inline |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| shim_spsc | 5 | 2.00 (2.00–2.00) | 1,588 | 2,348 | 2,996 | 21,121 | 245,696 | 276,774 | 1.93× | 1/1,403 | 1/201 | 1.00× |
| ring_sp | 5 | 2.00 (2.00–2.00) | 2,117 | 2,836 | 3,830 | 45,187 | 248,172 | 276,127 | 1.93× | 1/1,098 | 0.01× | 1.00× |
| ring_inline | 5 | 2.00 (2.00–2.00) | 555 | 708 | 1,108 | 4,746 | 203,978 | 275,938 | 1.93× | 1/3,794 | 1/893 | 1.00× |
| moodycamel_rw_sp | 5 | 2.00 (2.00–2.00) | 2,068 | 2,825 | 3,797 | 22,934 | 242,810 | 275,050 | 1.93× | 1/1,107 | 1/185 | 1.00× |
| rigtorp_sp | 5 | 2.00 (2.00–2.00) | 1,645 | 2,380 | 3,302 | 26,699 | 246,845 | 276,638 | 1.93× | 1/1,273 | 1/159 | 1.00× |
| shim_generic | 5 | 1.04 ⚠ (1.01–1.12) | 4,088,020 | 4,155,450 | 4,204,270 | 4,238,060 | 4,287,960 | 4,367,190 | 1.00× | 1.00× | 1.00× | 0.52× |
| orig_generic | 5 | 0.88 ⚠ (0.73–1.15) | 4,867,650 | 4,977,080 | 5,036,820 | 5,087,000 | 5,137,750 | 5,207,210 | 0.85× | 1.20× | 1.20× | 0.44× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
