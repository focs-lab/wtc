# w2  (2026-09-18T18:47:42Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;`, git d0a6b73c6b10 (dirty)

Control (shim_generic E64, median of 1 and 1): open 1.06, close 1.17 Mops/s, drift 9.5% — **NOISY: rerun this leg**

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.008, rep 2 0.954, rep 3 0.999, rep 4 1.040 (n=20) — no trend, but a wide spread (8.6%).  Widest single row: std_mutex E64 @1000, 86% across repetitions.

## W2 open  E64  wait=spin  interval=1000 ns

| variant | n | p50 | p90 | p99 | p99.9 | p99.99 | max | p99 vs shim_generic | p99.9 vs shim_generic |
|---|---|---|---|---|---|---|---|---|---|
| ring_inline | 5 | 184 | 306 | 335 (326–347) | 1,876 | 30,839 | 136,674 | 0.03× | 0.04× |
| ring_sp | 5 | 566 | 1,435 | 1,666 ⚠ (1,611–1,750) | 3,272 | 62,949 | 135,976 | 0.15× | 0.08× |
| shim_spsc | 5 | 579 | 1,552 | 1,716 ⚠ (1,670–1,847) | 3,485 | 38,266 | 90,506 | 0.16× | 0.08× |
| rigtorp_sp | 5 | 553 | 1,533 | 1,758 (1,702–1,802) | 3,444 | 6,235 | 47,964 | 0.16× | 0.08× |
| moodycamel_rw_sp | 5 | 696 | 1,542 | 1,772 ⚠ (1,706–1,960) | 3,403 | 5,642 | 51,412 | 0.16× | 0.08× |
| boost_spsc_sp | 5 | 630 | 1,617 | 1,867 ⚠ (1,741–2,008) | 3,628 | 11,760 | 51,886 | 0.17× | 0.09× |
| moodycamel_cq_sp | 5 | 879 | 1,780 | 2,072 ⚠ (1,995–2,324) | 4,563 | 14,993 | 48,005 | 0.19× | 0.11× |
| orig_generic | 5 | 2,124 | 3,809 | 7,306 ⚠ (5,948–171,662) | 16,212 | 123,698 | 143,677 | 0.67× | 0.39× |
| std_mutex | 5 | 1,741 | 5,095 | 10,125 ⚠ (7,811–21,544) | 15,505 | 121,625 | 195,243 | 0.93× | 0.37× |
| shim_generic | 5 | 2,742 | 6,244 | 10,906 ⚠ (7,057–13,169) | 41,808 | 127,593 | 419,160 | 1.00× | 1.00× |

## W2 open  E64  wait=spin  interval=10000 ns

| variant | n | p50 | p90 | p99 | p99.9 | p99.99 | max | p99 vs shim_generic | p99.9 vs shim_generic |
|---|---|---|---|---|---|---|---|---|---|
| ring_inline | 5 | 220 | 234 | 339 ⚠ (242–374) | 1,141 | 3,884 | 262,723 | 0.09× | 0.19× |
| ring_sp | 5 | 661 | 1,292 | 1,569 ⚠ (1,364–1,742) | 1,750 | 4,782 | 135,536 | 0.40× | 0.29× |
| rigtorp_sp | 5 | 585 | 1,310 | 1,575 ⚠ (1,275–1,892) | 1,678 | 4,807 | 39,372 | 0.40× | 0.28× |
| moodycamel_rw_sp | 5 | 600 | 1,290 | 1,616 ⚠ (1,389–1,845) | 1,705 | 4,874 | 124,166 | 0.41× | 0.28× |
| shim_spsc | 5 | 611 | 1,474 | 1,650 ⚠ (1,484–1,898) | 1,717 | 4,891 | 138,504 | 0.42× | 0.28× |
| moodycamel_cq_sp | 5 | 929 | 1,576 | 1,809 ⚠ (1,673–2,070) | 3,762 | 5,323 | 133,141 | 0.46× | 0.62× |
| boost_spsc_sp | 5 | 685 | 1,258 | 1,818 ⚠ (1,381–1,935) | 1,881 | 4,916 | 130,942 | 0.46× | 0.31× |
| std_mutex | 5 | 1,322 | 1,952 | 2,645 ⚠ (2,262–3,074) | 5,677 | 8,906 | 62,869 | 0.67× | 0.94× |
| shim_generic | 5 | 1,739 | 2,724 | 3,945 ⚠ (2,847–6,004) | 6,049 | 9,194 | 131,740 | 1.00× | 1.00× |
| orig_generic | 5 | 1,699 | 2,623 | 4,730 ⚠ (2,932–5,528) | 6,512 | 9,189 | 270,290 | 1.20× | 1.08× |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
