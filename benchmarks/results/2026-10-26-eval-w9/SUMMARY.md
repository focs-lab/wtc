# eval-w9  (2026-10-26T08:59:23Z)

AMD EPYC 9115 16-Core Processor, kernel 6.8.0-137-generic, allowed cpus 16-31,48-63, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=0;`, git 

Control (shim_generic E64, median of 3 and 3): open 15.41, close 15.17 Mops/s, drift 1.6% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.006, rep 2 1.010, rep 3 1.006, rep 4 1.009 (n=441) — no trend.  Widest single row: shim_generic in W9 split  E8  producers=8  consumers=1  wait=spin  alloc=jemalloc, 82% across repetitions.

## W9 pairwise  E8  threads=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 61.62 [60.92–62.53] | 16.23 | 1.21× |
| boost_ms_queue | 5 | 59.19 [58.20–59.30] | 16.89 | 1.16× |
| std_mutex | 5 | 56.44 [54.83–56.45] | 17.72 | 1.11× |
| faaarray | 5 | 51.92 [50.83–52.06] | 19.26 | 1.02× |
| shim_generic | 5 | 50.83 [50.75–50.87] | 19.67 | 1.00× |
| lcrq | 5 | 48.74 [47.52–49.39] | 20.52 | 0.96× |
| lprq | 5 | 40.15 [40.03–40.25] | 24.91 | 0.79× |
| lscq | 5 | 32.20 [32.09–32.22] | 31.05 | 0.63× |
| orig_generic | 5 | 27.01 [26.99–27.03] | 37.02 | 0.53× |

## W9 pairwise  E8  threads=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 67.51 [64.56–68.49] | 14.81 | 1.24× |
| std_mutex | 5 | 63.89 [62.03–64.10] | 15.65 | 1.17× |
| boost_ms_queue | 5 | 62.64 [62.15–64.27] | 15.96 | 1.15× |
| faaarray | 5 | 55.78 [54.41–55.81] | 17.93 | 1.02× |
| shim_generic | 5 | 54.62 [54.58–54.63] | 18.31 | 1.00× |
| lcrq | 5 | 52.63 [52.03–53.21] | 19.00 | 0.96× |
| lprq | 5 | 43.84 [43.38–43.85] | 22.81 | 0.80× |
| lscq | 5 | 33.60 [33.56–33.63] | 29.76 | 0.62× |
| orig_generic | 5 | 28.91 [28.89–28.93] | 34.59 | 0.53× |

## W9 pairwise  E8  threads=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 66.62 [64.06–67.06] | 15.01 | 1.20× |
| boost_ms_queue | 5 | 62.53 [62.24–62.94] | 15.99 | 1.12× |
| std_mutex | 5 | 62.04 [61.91–62.12] | 16.12 | 1.12× |
| shim_generic | 5 | 55.61 [54.84–55.69] | 17.98 | 1.00× |
| faaarray | 5 | 54.25 [53.16–54.68] | 18.43 | 0.98× |
| lcrq | 5 | 51.84 [50.79–52.16] | 19.29 | 0.93× |
| lprq | 5 | 42.89 [42.56–42.95] | 23.32 | 0.77× |
| lscq | 5 | 33.21 [33.20–33.25] | 30.11 | 0.60× |
| orig_generic | 5 | 29.08 [28.79–29.11] | 34.39 | 0.52× |

## W9 pairwise  E8  threads=16  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lprq | 5 | 32.26 ⚠ [26.90–32.34] | 31.00 | 1.48× |
| lcrq | 5 | 31.78 ⚠ [26.37–32.08] | 31.47 | 1.46× |
| faaarray | 5 | 29.82 ⚠ [24.56–29.97] | 33.54 | 1.37× |
| lscq | 5 | 23.02 ⚠ [17.44–23.05] | 43.44 | 1.06× |
| shim_generic | 5 | 21.81 ⚠ [19.09–22.03] | 45.86 | 1.00× |
| moodycamel_cq_sp | 5 | 12.13 ⚠ [9.65–12.21] | 82.43 | 0.56× |
| boost_ms_queue | 5 | 3.72 ⚠ [3.22–3.81] | 269 | 0.17× |
| std_mutex | 5 | 2.78 ⚠ [1.78–3.07] | 360 | 0.13× |
| orig_generic | 5 | 2.66 ⚠ [2.14–3.26] | 375 | 0.12× |

## W9 pairwise  E8  threads=16  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lcrq | 5 | 31.45 [30.55–31.91] | 31.80 | 1.45× |
| lprq | 5 | 31.13 ⚠ [26.05–31.42] | 32.12 | 1.44× |
| faaarray | 5 | 30.43 ⚠ [25.20–30.52] | 32.86 | 1.40× |
| lscq | 5 | 22.52 ⚠ [17.09–22.57] | 44.40 | 1.04× |
| shim_generic | 5 | 21.67 ⚠ [18.79–21.68] | 46.14 | 1.00× |
| moodycamel_cq_sp | 5 | 14.70 ⚠ [12.49–15.28] | 68.03 | 0.68× |
| boost_ms_queue | 5 | 3.50 ⚠ [3.00–3.54] | 285 | 0.16× |
| orig_generic | 5 | 3.50 ⚠ [2.94–3.94] | 286 | 0.16× |
| std_mutex | 5 | 3.11 ⚠ [2.19–3.47] | 322 | 0.14× |

## W9 pairwise  E8  threads=16  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lprq | 5 | 28.51 [28.31–28.82] | 35.07 | 1.34× |
| lcrq | 5 | 27.90 [27.73–28.52] | 35.84 | 1.31× |
| faaarray | 5 | 26.57 ⚠ [23.00–26.77] | 37.63 | 1.25× |
| shim_generic | 5 | 21.30 [20.77–21.45] | 46.96 | 1.00× |
| lscq | 5 | 21.21 [21.01–21.29] | 47.16 | 1.00× |
| moodycamel_cq_sp | 5 | 13.68 [13.52–14.17] | 73.11 | 0.64× |
| boost_ms_queue | 5 | 3.28 [3.27–3.36] | 304 | 0.15× |
| std_mutex | 5 | 2.52 ⚠ [2.45–2.70] | 397 | 0.12× |
| orig_generic | 5 | 2.43 ⚠ [1.89–2.66] | 412 | 0.11× |

## W9 pairwise  E8  threads=2  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 32.09 [31.65–32.25] | 31.16 | 1.30× |
| faaarray | 5 | 32.05 [31.66–32.29] | 31.20 | 1.30× |
| lcrq | 5 | 25.71 [25.25–25.88] | 38.90 | 1.04× |
| shim_generic | 5 | 24.74 [24.20–25.20] | 40.41 | 1.00× |
| lprq | 5 | 24.54 [24.48–24.68] | 40.75 | 0.99× |
| lscq | 5 | 17.30 [17.15–17.42] | 57.80 | 0.70× |
| orig_generic | 5 | 13.35 [12.87–13.65] | 74.90 | 0.54× |
| boost_ms_queue | 5 | 13.18 [13.12–13.79] | 75.85 | 0.53× |
| std_mutex | 5 | 10.88 ⚠ [10.03–12.12] | 91.93 | 0.44× |

## W9 pairwise  E8  threads=2  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 38.32 [37.79–39.21] | 26.10 | 1.53× |
| faaarray | 5 | 28.66 [28.47–28.77] | 34.90 | 1.15× |
| lprq | 5 | 25.98 [25.77–26.16] | 38.49 | 1.04× |
| lcrq | 5 | 25.67 [25.37–26.33] | 38.96 | 1.03× |
| shim_generic | 5 | 25.02 [24.80–25.39] | 39.97 | 1.00× |
| lscq | 5 | 17.53 [17.45–17.60] | 57.06 | 0.70× |
| orig_generic | 5 | 14.01 [13.89–14.75] | 71.40 | 0.56× |
| boost_ms_queue | 5 | 12.82 [12.71–12.93] | 78.01 | 0.51× |
| std_mutex | 5 | 11.06 ⚠ [10.77–11.92] | 90.40 | 0.44× |

## W9 pairwise  E8  threads=2  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 38.61 [37.49–40.35] | 25.90 | 1.50× |
| faaarray | 5 | 27.52 [26.80–27.58] | 36.33 | 1.07× |
| shim_generic | 5 | 25.70 [25.17–25.74] | 38.91 | 1.00× |
| lcrq | 5 | 23.95 [23.87–24.23] | 41.76 | 0.93× |
| lprq | 5 | 23.26 [23.07–23.37] | 42.98 | 0.91× |
| lscq | 5 | 16.76 [16.66–16.82] | 59.67 | 0.65× |
| orig_generic | 5 | 13.15 ⚠ [12.91–14.26] | 76.05 | 0.51× |
| std_mutex | 5 | 12.57 ⚠ [11.06–12.98] | 79.54 | 0.49× |
| boost_ms_queue | 5 | 12.45 [12.28–12.64] | 80.34 | 0.48× |

## W9 pairwise  E8  threads=4  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| faaarray | 5 | 37.44 [37.22–37.58] | 26.71 | 1.28× |
| lcrq | 5 | 36.30 [36.08–36.59] | 27.54 | 1.24× |
| lprq | 5 | 35.40 [33.71–36.17] | 28.25 | 1.21× |
| shim_generic | 5 | 29.36 [29.06–29.42] | 34.06 | 1.00× |
| lscq | 5 | 26.87 [25.55–26.98] | 37.21 | 0.92× |
| moodycamel_cq_sp | 5 | 23.71 [23.69–23.80] | 42.17 | 0.81× |
| std_mutex | 5 | 18.55 ⚠ [16.80–20.19] | 53.92 | 0.63× |
| orig_generic | 5 | 11.29 [10.87–11.39] | 88.58 | 0.38× |
| boost_ms_queue | 5 | 9.12 ⚠ [9.04–9.88] | 110 | 0.31× |

## W9 pairwise  E8  threads=4  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| faaarray | 5 | 38.15 [37.88–38.58] | 26.21 | 1.26× |
| lprq | 5 | 37.16 [35.51–37.63] | 26.91 | 1.22× |
| lcrq | 5 | 36.45 [36.06–38.26] | 27.43 | 1.20× |
| shim_generic | 5 | 30.35 [30.19–30.39] | 32.95 | 1.00× |
| lscq | 5 | 26.63 [26.37–26.78] | 37.55 | 0.88× |
| moodycamel_cq_sp | 5 | 25.68 [25.34–26.24] | 38.94 | 0.85× |
| std_mutex | 5 | 21.80 [20.88–22.35] | 45.88 | 0.72× |
| orig_generic | 5 | 11.91 [11.82–12.05] | 83.95 | 0.39× |
| boost_ms_queue | 5 | 9.60 [9.38–9.86] | 104 | 0.32× |

## W9 pairwise  E8  threads=4  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| faaarray | 5 | 35.19 [35.16–35.28] | 28.42 | 1.18× |
| lcrq | 5 | 34.30 [33.91–34.58] | 29.15 | 1.15× |
| lprq | 5 | 33.33 [33.07–33.45] | 30.00 | 1.12× |
| shim_generic | 5 | 29.89 [29.48–29.90] | 33.46 | 1.00× |
| moodycamel_cq_sp | 5 | 24.93 [24.61–25.32] | 40.11 | 0.83× |
| lscq | 5 | 24.22 [24.13–24.41] | 41.28 | 0.81× |
| std_mutex | 5 | 17.87 ⚠ [16.01–18.74] | 55.97 | 0.60× |
| orig_generic | 5 | 9.83 [9.74–10.11] | 102 | 0.33× |
| boost_ms_queue | 5 | 8.91 [8.88–9.44] | 112 | 0.30× |

## W9 pairwise  E8  threads=8  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lprq | 5 | 44.36 [44.22–45.93] | 22.54 | 1.44× |
| lcrq | 5 | 44.28 [43.16–44.40] | 22.58 | 1.43× |
| faaarray | 5 | 43.75 [43.22–43.92] | 22.85 | 1.42× |
| lscq | 5 | 37.13 [37.00–37.19] | 26.94 | 1.20× |
| shim_generic | 5 | 30.90 [30.88–31.45] | 32.36 | 1.00× |
| moodycamel_cq_sp | 5 | 26.00 [25.72–26.50] | 38.47 | 0.84× |
| orig_generic | 5 | 8.35 [8.15–8.44] | 120 | 0.27× |
| std_mutex | 5 | 8.26 [8.22–8.59] | 121 | 0.27× |
| boost_ms_queue | 5 | 7.07 [6.91–7.09] | 141 | 0.23× |

## W9 pairwise  E8  threads=8  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lcrq | 5 | 43.83 [43.60–46.22] | 22.82 | 1.42× |
| faaarray | 5 | 43.72 [43.43–44.18] | 22.87 | 1.42× |
| lprq | 5 | 43.42 [42.06–43.99] | 23.03 | 1.41× |
| lscq | 5 | 35.76 [35.42–35.85] | 27.96 | 1.16× |
| shim_generic | 5 | 30.83 [30.70–30.91] | 32.44 | 1.00× |
| moodycamel_cq_sp | 5 | 26.27 [25.65–26.33] | 38.06 | 0.85× |
| std_mutex | 5 | 8.70 [8.64–8.73] | 115 | 0.28× |
| orig_generic | 5 | 8.59 [8.46–8.71] | 116 | 0.28× |
| boost_ms_queue | 5 | 7.05 [6.96–7.09] | 142 | 0.23× |

## W9 pairwise  E8  threads=8  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lcrq | 5 | 40.09 [40.08–40.29] | 24.94 | 1.31× |
| lprq | 5 | 39.61 [39.41–40.00] | 25.24 | 1.29× |
| faaarray | 5 | 39.07 [38.65–39.18] | 25.59 | 1.28× |
| lscq | 5 | 33.37 [33.05–33.43] | 29.97 | 1.09× |
| shim_generic | 5 | 30.63 [30.50–30.69] | 32.65 | 1.00× |
| moodycamel_cq_sp | 5 | 25.91 [24.52–26.48] | 38.59 | 0.85× |
| std_mutex | 5 | 7.41 [7.27–7.42] | 135 | 0.24× |
| orig_generic | 5 | 7.16 [6.97–7.31] | 140 | 0.23× |
| boost_ms_queue | 5 | 6.60 [6.59–6.67] | 152 | 0.22× |

## W9 split  E8  producers=1  consumers=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 29.41 [28.57–29.58] | 34.01 | 2.05× |
| shim_spmc | 5 | 28.00 [27.98–28.00] | 35.72 | 1.95× |
| shim_generic | 5 | 14.37 [14.33–14.42] | 69.58 | 1.00× |
| lcrq | 5 | 9.80 ⚠ [6.95–9.89] | 102 | 0.68× |
| lscq | 5 | 9.58 ⚠ [6.11–9.69] | 104 | 0.67× |
| moodycamel_cq_sp | 5 | 7.78 [7.76–7.88] | 129 | 0.54× |
| lprq | 5 | 7.78 ⚠ [5.63–8.04] | 129 | 0.54× |
| orig_mpsc | 5 | 6.89 [6.71–7.07] | 145 | 0.48× |
| faaarray | 5 | 6.44 ⚠ [6.12–7.44] | 155 | 0.45× |
| boost_ms_queue | 5 | 5.66 [5.56–5.70] | 177 | 0.39× |
| std_mutex | 5 | 5.32 ⚠ [4.97–5.60] | 188 | 0.37× |
| orig_generic | 5 | 4.24 [4.12–4.28] | 236 | 0.30× |

## W9 split  E8  producers=1  consumers=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 30.15 [29.80–30.24] | 33.17 | 2.01× |
| shim_mpsc | 5 | 29.48 [28.65–29.55] | 33.92 | 1.97× |
| orig_mpsc | 5 | 23.78 ⚠ [19.43–24.04] | 42.06 | 1.59× |
| lcrq | 5 | 17.76 ⚠ [17.46–20.84] | 56.30 | 1.19× |
| shim_generic | 5 | 14.99 [14.66–15.55] | 66.72 | 1.00× |
| lscq | 5 | 14.67 [14.63–14.78] | 68.15 | 0.98× |
| moodycamel_cq_sp | 5 | 14.25 [14.22–14.30] | 70.16 | 0.95× |
| lprq | 5 | 12.16 [11.32–12.30] | 82.22 | 0.81× |
| std_mutex | 5 | 10.50 [10.40–10.85] | 95.27 | 0.70× |
| faaarray | 5 | 9.42 ⚠ [9.15–12.44] | 106 | 0.63× |
| boost_ms_queue | 5 | 6.89 [6.72–7.16] | 145 | 0.46× |
| orig_generic | 5 | 6.44 ⚠ [6.01–7.05] | 155 | 0.43× |

## W9 split  E8  producers=1  consumers=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 30.14 [29.18–30.17] | 33.18 | 2.08× |
| shim_mpsc | 5 | 29.54 [28.53–29.59] | 33.86 | 2.04× |
| shim_generic | 5 | 14.49 [14.21–15.44] | 69.03 | 1.00× |
| moodycamel_cq_sp | 5 | 13.09 ⚠ [10.71–13.16] | 76.37 | 0.90× |
| std_mutex | 5 | 10.61 ⚠ [8.89–11.07] | 94.21 | 0.73× |
| lcrq | 5 | 9.96 [9.83–10.13] | 100 | 0.69× |
| lscq | 5 | 8.87 ⚠ [6.80–9.02] | 113 | 0.61× |
| orig_mpsc | 5 | 8.53 ⚠ [8.33–13.54] | 117 | 0.59× |
| lprq | 5 | 8.28 [8.10–8.69] | 121 | 0.57× |
| faaarray | 5 | 7.62 [7.19–7.73] | 131 | 0.53× |
| boost_ms_queue | 5 | 5.92 [5.79–6.00] | 169 | 0.41× |
| orig_generic | 5 | 4.56 ⚠ [4.52–4.96] | 219 | 0.32× |

## W9 split  E8  producers=1  consumers=2  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 25.53 ⚠ [24.92–32.33] | 39.17 | 2.43× |
| shim_generic | 5 | 10.49 [10.47–10.72] | 95.29 | 1.00× |
| moodycamel_cq_sp | 5 | 7.50 [7.45–7.54] | 133 | 0.71× |
| lcrq | 5 | 5.55 ⚠ [5.00–5.60] | 180 | 0.53× |
| lprq | 5 | 5.05 [4.99–5.06] | 198 | 0.48× |
| boost_ms_queue | 5 | 4.77 [4.57–4.80] | 210 | 0.45× |
| orig_generic | 5 | 4.28 ⚠ [4.03–4.31] | 234 | 0.41× |
| lscq | 5 | 4.18 ⚠ [3.82–4.18] | 239 | 0.40× |
| std_mutex | 5 | 3.45 ⚠ [3.27–3.65] | 290 | 0.33× |
| faaarray | 5 | 2.80 ⚠ [2.62–3.29] | 357 | 0.27× |

## W9 split  E8  producers=1  consumers=2  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 30.64 ⚠ [25.04–32.84] | 32.64 | 2.86× |
| moodycamel_cq_sp | 5 | 15.23 [15.07–15.30] | 65.66 | 1.42× |
| shim_generic | 5 | 10.71 [10.62–10.87] | 93.35 | 1.00× |
| lcrq | 5 | 7.30 ⚠ [7.17–8.02] | 137 | 0.68× |
| lprq | 5 | 6.11 [5.73–6.14] | 164 | 0.57× |
| orig_generic | 5 | 5.77 [5.45–5.81] | 173 | 0.54× |
| boost_ms_queue | 5 | 5.10 [5.06–5.41] | 196 | 0.48× |
| lscq | 5 | 4.56 ⚠ [4.47–5.00] | 220 | 0.43× |
| std_mutex | 5 | 4.23 [4.08–4.43] | 236 | 0.40× |
| faaarray | 5 | 3.42 [3.28–3.47] | 292 | 0.32× |

## W9 split  E8  producers=1  consumers=2  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 29.91 ⚠ [24.83–33.06] | 33.43 | 2.79× |
| shim_generic | 5 | 10.71 [10.64–10.78] | 93.33 | 1.00× |
| moodycamel_cq_sp | 5 | 10.66 ⚠ [9.34–10.84] | 93.82 | 0.99× |
| orig_generic | 5 | 5.77 ⚠ [5.30–5.87] | 173 | 0.54× |
| lcrq | 5 | 5.41 [5.15–5.47] | 185 | 0.50× |
| lprq | 5 | 4.84 [4.78–4.89] | 207 | 0.45× |
| boost_ms_queue | 5 | 4.67 [4.50–4.76] | 214 | 0.44× |
| std_mutex | 5 | 3.93 [3.82–4.04] | 255 | 0.37× |
| lscq | 5 | 3.63 [3.58–3.69] | 275 | 0.34× |
| faaarray | 5 | 3.34 ⚠ [3.32–4.04] | 299 | 0.31× |

## W9 split  E8  producers=1  consumers=4  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 14.70 [14.19–14.86] | 68.05 | 1.62× |
| shim_generic | 5 | 9.08 [8.83–9.12] | 110 | 1.00× |
| moodycamel_cq_sp | 5 | 6.56 [6.50–6.59] | 152 | 0.72× |
| lcrq | 5 | 4.07 [4.04–4.13] | 246 | 0.45× |
| orig_generic | 5 | 3.90 [3.85–3.92] | 256 | 0.43× |
| lprq | 5 | 3.81 [3.75–3.84] | 263 | 0.42× |
| boost_ms_queue | 5 | 3.56 [3.42–3.57] | 281 | 0.39× |
| lscq | 5 | 2.55 ⚠ [2.28–2.55] | 393 | 0.28× |
| std_mutex | 5 | 1.74 [1.69–1.78] | 573 | 0.19× |
| faaarray | 5 | 1.11 [1.10–1.13] | 897 | 0.12× |

## W9 split  E8  producers=1  consumers=4  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 15.51 ⚠ [14.78–16.95] | 64.47 | 1.70× |
| moodycamel_cq_sp | 5 | 11.82 [11.79–11.87] | 84.59 | 1.29× |
| shim_generic | 5 | 9.14 [9.09–9.16] | 109 | 1.00× |
| orig_generic | 5 | 5.08 [5.01–5.11] | 197 | 0.56× |
| lcrq | 5 | 4.47 [4.39–4.57] | 224 | 0.49× |
| lprq | 5 | 4.13 [4.11–4.18] | 242 | 0.45× |
| boost_ms_queue | 5 | 4.02 [3.94–4.04] | 249 | 0.44× |
| lscq | 5 | 2.69 [2.53–2.73] | 372 | 0.29× |
| std_mutex | 5 | 2.00 ⚠ [1.86–2.03] | 499 | 0.22× |
| faaarray | 5 | 1.17 [1.17–1.17] | 853 | 0.13× |

## W9 split  E8  producers=1  consumers=4  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 16.24 ⚠ [14.85–16.91] | 61.57 | 1.77× |
| shim_generic | 5 | 9.20 [9.03–9.26] | 109 | 1.00× |
| moodycamel_cq_sp | 5 | 7.77 [7.58–8.12] | 129 | 0.84× |
| orig_generic | 5 | 5.03 [4.82–5.10] | 199 | 0.55× |
| lcrq | 5 | 3.67 [3.57–3.71] | 273 | 0.40× |
| boost_ms_queue | 5 | 3.55 [3.51–3.58] | 282 | 0.39× |
| lprq | 5 | 3.44 [3.40–3.51] | 291 | 0.37× |
| lscq | 5 | 2.35 [2.26–2.39] | 426 | 0.26× |
| std_mutex | 5 | 1.87 [1.84–1.92] | 535 | 0.20× |
| faaarray | 5 | 1.22 [1.20–1.24] | 822 | 0.13× |

## W9 split  E8  producers=1  consumers=8  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 4.85 ⚠ [4.58–6.71] | 206 | 2.29× |
| moodycamel_cq_sp | 5 | 2.90 ⚠ [2.56–2.94] | 345 | 1.37× |
| shim_generic | 5 | 2.12 ⚠ [1.51–2.20] | 472 | 1.00× |
| orig_generic | 5 | 2.12 ⚠ [1.71–2.21] | 473 | 1.00× |
| lcrq | 5 | 1.66 ⚠ [1.49–1.75] | 603 | 0.78× |
| lprq | 5 | 1.51 ⚠ [1.30–1.59] | 664 | 0.71× |
| boost_ms_queue | 5 | 1.30 ⚠ [1.11–1.34] | 767 | 0.62× |
| lscq | 5 | 0.95 [0.94–0.98] | 1,057 | 0.45× |
| std_mutex | 5 | 0.56 ⚠ [0.47–0.57] | 1,799 | 0.26× |
| faaarray | 5 | 0.37 ⚠ [0.35–0.48] | 2,686 | 0.18× |

## W9 split  E8  producers=1  consumers=8  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 4.95 ⚠ [4.52–5.32] | 202 | 2.31× |
| moodycamel_cq_sp | 5 | 3.88 ⚠ [3.43–3.91] | 258 | 1.81× |
| orig_generic | 5 | 2.48 ⚠ [2.14–2.55] | 404 | 1.16× |
| shim_generic | 5 | 2.14 ⚠ [1.47–2.22] | 467 | 1.00× |
| lcrq | 5 | 1.63 ⚠ [1.22–1.65] | 614 | 0.76× |
| lprq | 5 | 1.51 ⚠ [1.08–1.63] | 660 | 0.71× |
| boost_ms_queue | 5 | 1.29 ⚠ [0.95–1.35] | 773 | 0.60× |
| lscq | 5 | 0.97 ⚠ [0.84–1.04] | 1,028 | 0.45× |
| std_mutex | 5 | 0.58 ⚠ [0.44–0.59] | 1,725 | 0.27× |
| faaarray | 5 | 0.39 ⚠ [0.34–0.40] | 2,534 | 0.18× |

## W9 split  E8  producers=1  consumers=8  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 5.59 [5.28–5.72] | 179 | 2.59× |
| moodycamel_cq_sp | 5 | 3.07 ⚠ [2.78–3.34] | 326 | 1.42× |
| orig_generic | 5 | 2.48 ⚠ [2.12–2.53] | 403 | 1.15× |
| shim_generic | 5 | 2.16 ⚠ [1.41–2.18] | 464 | 1.00× |
| lcrq | 5 | 1.52 ⚠ [1.21–1.63] | 659 | 0.70× |
| lprq | 5 | 1.44 ⚠ [1.12–1.48] | 693 | 0.67× |
| boost_ms_queue | 5 | 1.22 ⚠ [0.95–1.28] | 821 | 0.57× |
| lscq | 5 | 0.94 ⚠ [0.85–0.96] | 1,065 | 0.44× |
| std_mutex | 5 | 0.56 ⚠ [0.49–0.60] | 1,775 | 0.26× |
| faaarray | 5 | 0.38 [0.37–0.40] | 2,610 | 0.18× |

## W9 split  E8  producers=2  consumers=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 29.84 [28.64–29.92] | 33.52 | 1.19× |
| shim_generic | 5 | 25.14 [25.08–25.16] | 39.78 | 1.00× |
| lprq | 5 | 13.89 [13.51–14.06] | 72.01 | 0.55× |
| lcrq | 5 | 13.81 [13.21–14.20] | 72.40 | 0.55× |
| faaarray | 5 | 12.12 [11.89–12.51] | 82.53 | 0.48× |
| lscq | 5 | 11.03 [10.97–11.15] | 90.67 | 0.44× |
| orig_mpsc | 5 | 10.79 [10.35–11.10] | 92.68 | 0.43× |
| moodycamel_cq_sp | 5 | 10.56 [10.54–10.57] | 94.71 | 0.42× |
| boost_ms_queue | 5 | 7.43 [7.41–7.46] | 135 | 0.30× |
| std_mutex | 5 | 6.16 [6.06–6.21] | 162 | 0.25× |
| orig_generic | 5 | 4.43 [4.35–4.61] | 226 | 0.18× |

## W9 split  E8  producers=2  consumers=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 30.02 ⚠ [21.81–30.11] | 33.32 | 1.12× |
| shim_generic | 5 | 26.87 ⚠ [21.26–26.89] | 37.22 | 1.00× |
| orig_mpsc | 5 | 23.06 ⚠ [17.39–24.00] | 43.37 | 0.86× |
| faaarray | 5 | 18.96 ⚠ [16.34–19.06] | 52.74 | 0.71× |
| lcrq | 5 | 18.82 ⚠ [17.42–19.01] | 53.13 | 0.70× |
| lprq | 5 | 17.25 ⚠ [15.39–17.38] | 57.96 | 0.64× |
| moodycamel_cq_sp | 5 | 15.11 ⚠ [13.50–15.15] | 66.16 | 0.56× |
| lscq | 5 | 13.51 ⚠ [11.83–14.05] | 74.00 | 0.50× |
| std_mutex | 5 | 8.41 ⚠ [7.19–8.61] | 119 | 0.31× |
| boost_ms_queue | 5 | 7.70 ⚠ [7.14–8.04] | 130 | 0.29× |
| orig_generic | 5 | 5.34 ⚠ [4.99–5.62] | 187 | 0.20× |

## W9 split  E8  producers=2  consumers=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 32.35 [32.21–32.37] | 30.91 | 1.18× |
| shim_generic | 5 | 27.41 [27.35–27.43] | 36.48 | 1.00× |
| orig_mpsc | 5 | 18.77 [18.71–18.97] | 53.29 | 0.68× |
| lcrq | 5 | 15.07 ⚠ [13.60–15.89] | 66.38 | 0.55× |
| moodycamel_cq_sp | 5 | 14.01 ⚠ [13.16–14.44] | 71.38 | 0.51× |
| faaarray | 5 | 12.90 ⚠ [12.38–13.48] | 77.50 | 0.47× |
| lprq | 5 | 12.36 ⚠ [12.13–13.61] | 80.88 | 0.45× |
| lscq | 5 | 9.97 [9.77–10.08] | 100 | 0.36× |
| std_mutex | 5 | 8.03 [8.00–8.24] | 125 | 0.29× |
| boost_ms_queue | 5 | 7.13 [7.12–7.27] | 140 | 0.26× |
| orig_generic | 5 | 5.44 [5.33–5.52] | 184 | 0.20× |

## W9 split  E8  producers=2  consumers=2  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 22.68 [22.37–22.72] | 44.08 | 1.00× |
| lcrq | 5 | 12.10 [11.89–12.29] | 82.63 | 0.53× |
| lprq | 5 | 10.73 [10.62–10.91] | 93.22 | 0.47× |
| moodycamel_cq_sp | 5 | 9.63 [9.49–9.68] | 104 | 0.42× |
| faaarray | 5 | 7.81 [7.60–7.83] | 128 | 0.34× |
| lscq | 5 | 7.68 [7.50–7.91] | 130 | 0.34× |
| boost_ms_queue | 5 | 5.94 [5.57–6.01] | 168 | 0.26× |
| std_mutex | 5 | 4.48 [4.35–4.63] | 223 | 0.20× |
| orig_generic | 5 | 3.90 [3.80–3.91] | 257 | 0.17× |

## W9 split  E8  producers=2  consumers=2  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 22.39 [22.03–22.83] | 44.67 | 1.00× |
| lcrq | 5 | 14.07 ⚠ [13.36–15.17] | 71.06 | 0.63× |
| moodycamel_cq_sp | 5 | 13.81 [13.77–13.91] | 72.43 | 0.62× |
| lprq | 5 | 12.52 [12.32–13.02] | 79.85 | 0.56× |
| faaarray | 5 | 11.18 [10.94–11.50] | 89.46 | 0.50× |
| lscq | 5 | 10.99 ⚠ [10.05–12.10] | 91.01 | 0.49× |
| boost_ms_queue | 5 | 5.90 [5.88–6.22] | 169 | 0.26× |
| std_mutex | 5 | 5.57 [5.31–5.64] | 180 | 0.25× |
| orig_generic | 5 | 4.79 [4.73–4.87] | 209 | 0.21× |

## W9 split  E8  producers=2  consumers=2  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 22.73 [21.88–23.90] | 43.99 | 1.00× |
| moodycamel_cq_sp | 5 | 12.15 [11.94–12.20] | 82.31 | 0.53× |
| lcrq | 5 | 9.89 [9.80–10.00] | 101 | 0.43× |
| lprq | 5 | 9.08 [9.05–9.13] | 110 | 0.40× |
| faaarray | 5 | 7.57 ⚠ [7.41–8.27] | 132 | 0.33× |
| lscq | 5 | 6.12 [6.09–6.17] | 163 | 0.27× |
| boost_ms_queue | 5 | 5.94 [5.53–5.97] | 168 | 0.26× |
| std_mutex | 5 | 5.52 [5.37–5.59] | 181 | 0.24× |
| orig_generic | 5 | 4.78 [4.66–4.83] | 209 | 0.21× |

## W9 split  E8  producers=4  consumers=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 29.83 [28.62–29.89] | 33.52 | 1.20× |
| shim_generic | 5 | 24.83 [24.68–24.86] | 40.27 | 1.00× |
| lcrq | 5 | 13.98 [13.40–14.27] | 71.53 | 0.56× |
| lscq | 5 | 13.73 [13.24–13.84] | 72.84 | 0.55× |
| moodycamel_cq_sp | 5 | 13.60 [13.55–13.63] | 73.56 | 0.55× |
| lprq | 5 | 13.23 [13.01–13.64] | 75.60 | 0.53× |
| faaarray | 5 | 12.83 [12.54–12.96] | 77.95 | 0.52× |
| orig_mpsc | 5 | 10.53 [10.37–10.70] | 94.93 | 0.42× |
| boost_ms_queue | 5 | 6.84 [6.55–6.96] | 146 | 0.28× |
| std_mutex | 5 | 5.41 [5.17–5.46] | 185 | 0.22× |
| orig_generic | 5 | 4.10 [4.08–4.13] | 244 | 0.17× |

## W9 split  E8  producers=4  consumers=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 29.82 ⚠ [22.90–29.87] | 33.54 | 1.12× |
| shim_generic | 5 | 26.73 ⚠ [21.47–26.77] | 37.41 | 1.00× |
| orig_mpsc | 5 | 22.50 ⚠ [17.41–22.73] | 44.44 | 0.84× |
| lcrq | 5 | 20.30 ⚠ [15.83–20.41] | 49.26 | 0.76× |
| lprq | 5 | 20.15 ⚠ [16.25–20.78] | 49.63 | 0.75× |
| faaarray | 5 | 18.90 ⚠ [15.82–19.25] | 52.91 | 0.71× |
| lscq | 5 | 16.85 ⚠ [14.00–16.91] | 59.35 | 0.63× |
| moodycamel_cq_sp | 5 | 16.05 ⚠ [13.72–16.27] | 62.29 | 0.60× |
| boost_ms_queue | 5 | 7.26 ⚠ [5.67–7.34] | 138 | 0.27× |
| std_mutex | 5 | 6.23 ⚠ [5.48–6.30] | 160 | 0.23× |
| orig_generic | 5 | 4.48 ⚠ [4.19–4.67] | 223 | 0.17× |

## W9 split  E8  producers=4  consumers=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 32.24 [31.42–32.31] | 31.02 | 1.17× |
| shim_generic | 5 | 27.47 [27.45–27.48] | 36.40 | 1.00× |
| orig_mpsc | 5 | 19.51 [19.38–19.78] | 51.26 | 0.71× |
| moodycamel_cq_sp | 5 | 14.62 [14.58–14.67] | 68.38 | 0.53× |
| faaarray | 5 | 14.02 [13.63–14.08] | 71.34 | 0.51× |
| lcrq | 5 | 13.74 [13.55–13.79] | 72.75 | 0.50× |
| lscq | 5 | 13.55 ⚠ [12.50–13.95] | 73.82 | 0.49× |
| lprq | 5 | 13.35 [13.31–13.61] | 74.91 | 0.49× |
| boost_ms_queue | 5 | 7.04 ⚠ [6.56–7.14] | 142 | 0.26× |
| std_mutex | 5 | 6.10 [6.06–6.15] | 164 | 0.22× |
| orig_generic | 5 | 4.76 [4.54–4.79] | 210 | 0.17× |

## W9 split  E8  producers=4  consumers=4  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 26.04 [25.87–26.09] | 38.40 | 1.00× |
| lcrq | 5 | 15.62 [14.74–15.93] | 64.04 | 0.60× |
| lprq | 5 | 14.93 [14.19–15.09] | 66.99 | 0.57× |
| moodycamel_cq_sp | 5 | 12.99 [12.90–13.01] | 76.98 | 0.50× |
| lscq | 5 | 10.65 [10.51–10.90] | 93.88 | 0.41× |
| faaarray | 5 | 6.74 ⚠ [6.49–7.65] | 148 | 0.26× |
| boost_ms_queue | 5 | 4.18 [4.05–4.21] | 239 | 0.16× |
| orig_generic | 5 | 3.35 [3.32–3.38] | 298 | 0.13× |
| std_mutex | 5 | 3.24 [3.21–3.29] | 309 | 0.12× |

## W9 split  E8  producers=4  consumers=4  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 46.56 [46.53–46.92] | 21.48 | 1.00× |
| moodycamel_cq_sp | 5 | 17.68 [16.82–17.76] | 56.58 | 0.38× |
| lcrq | 5 | 15.99 ⚠ [15.25–17.65] | 62.52 | 0.34× |
| lprq | 5 | 15.13 [14.98–15.98] | 66.08 | 0.33× |
| lscq | 5 | 12.41 [11.95–12.95] | 80.60 | 0.27× |
| faaarray | 5 | 10.47 [10.37–11.09] | 95.52 | 0.22× |
| boost_ms_queue | 5 | 4.26 [4.11–4.27] | 234 | 0.09× |
| orig_generic | 5 | 3.90 [3.85–3.94] | 256 | 0.08× |
| std_mutex | 5 | 3.51 ⚠ [3.14–3.58] | 285 | 0.08× |

## W9 split  E8  producers=4  consumers=4  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 47.08 [46.61–47.13] | 21.24 | 1.00× |
| moodycamel_cq_sp | 5 | 16.07 ⚠ [15.12–17.19] | 62.21 | 0.34× |
| lcrq | 5 | 11.94 [11.64–12.44] | 83.75 | 0.25× |
| lprq | 5 | 11.54 [11.35–11.59] | 86.62 | 0.25× |
| lscq | 5 | 8.56 [8.47–8.63] | 117 | 0.18× |
| faaarray | 5 | 6.82 [6.61–6.87] | 147 | 0.14× |
| boost_ms_queue | 5 | 4.25 [4.09–4.26] | 235 | 0.09× |
| orig_generic | 5 | 3.71 [3.66–3.84] | 270 | 0.08× |
| std_mutex | 5 | 3.39 [3.39–3.42] | 295 | 0.07× |

## W9 split  E8  producers=8  consumers=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 25.75 ⚠ [20.14–25.77] | 38.84 | 1.08× |
| shim_generic | 5 | 23.89 ⚠ [13.61–24.01] | 41.85 | 1.00× |
| lcrq | 5 | 9.90 ⚠ [8.38–10.02] | 101 | 0.41× |
| faaarray | 5 | 9.90 ⚠ [6.32–10.01] | 101 | 0.41× |
| lprq | 5 | 9.48 ⚠ [7.87–10.04] | 105 | 0.40× |
| moodycamel_cq_sp | 5 | 8.60 ⚠ [6.04–8.90] | 116 | 0.36× |
| lscq | 5 | 7.26 ⚠ [5.60–7.57] | 138 | 0.30× |
| orig_mpsc | 5 | 3.82 ⚠ [2.44–3.89] | 261 | 0.16× |
| std_mutex | 5 | 2.65 ⚠ [2.01–2.67] | 377 | 0.11× |
| boost_ms_queue | 5 | 2.00 ⚠ [1.36–2.02] | 501 | 0.08× |
| orig_generic | 5 | 1.71 ⚠ [1.59–1.78] | 583 | 0.07× |

## W9 split  E8  producers=8  consumers=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 25.72 ⚠ [14.17–25.84] | 38.87 | 1.00× |
| shim_mpsc | 5 | 25.70 ⚠ [20.53–25.80] | 38.91 | 1.00× |
| lprq | 5 | 14.22 ⚠ [11.76–14.35] | 70.30 | 0.55× |
| lcrq | 5 | 13.88 ⚠ [11.52–14.05] | 72.05 | 0.54× |
| moodycamel_cq_sp | 5 | 13.50 ⚠ [11.92–13.67] | 74.08 | 0.52× |
| orig_mpsc | 5 | 13.49 ⚠ [7.59–13.82] | 74.14 | 0.52× |
| faaarray | 5 | 13.03 ⚠ [11.10–13.59] | 76.74 | 0.51× |
| lscq | 5 | 12.43 ⚠ [10.69–12.46] | 80.45 | 0.48× |
| std_mutex | 5 | 3.55 ⚠ [2.73–3.57] | 282 | 0.14× |
| orig_generic | 5 | 2.74 ⚠ [2.17–2.77] | 365 | 0.11× |
| boost_ms_queue | 5 | 2.38 ⚠ [1.63–2.39] | 421 | 0.09× |

## W9 split  E8  producers=8  consumers=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 25.67 [25.61–25.82] | 38.96 | 1.00× |
| shim_mpsc | 5 | 25.45 [25.34–25.51] | 39.29 | 0.99× |
| moodycamel_cq_sp | 5 | 10.25 [9.93–10.55] | 97.56 | 0.40× |
| orig_mpsc | 5 | 9.99 [9.94–10.21] | 100 | 0.39× |
| lprq | 5 | 9.50 [9.46–9.58] | 105 | 0.37× |
| faaarray | 5 | 9.45 [9.29–9.92] | 106 | 0.37× |
| lcrq | 5 | 9.35 ⚠ [7.91–9.48] | 107 | 0.36× |
| lscq | 5 | 7.80 [7.73–7.84] | 128 | 0.30× |
| std_mutex | 5 | 2.94 [2.87–2.97] | 340 | 0.11× |
| boost_ms_queue | 5 | 2.23 ⚠ [1.89–2.25] | 448 | 0.09× |
| orig_generic | 5 | 2.20 [2.15–2.27] | 454 | 0.09× |

## W9 split  E8  producers=8  consumers=8  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 15.94 ⚠ [11.32–17.39] | 62.74 | 1.00× |
| lcrq | 5 | 15.68 ⚠ [11.98–15.97] | 63.79 | 0.98× |
| lprq | 5 | 15.53 ⚠ [9.99–16.49] | 64.37 | 0.97× |
| moodycamel_cq_sp | 5 | 11.23 ⚠ [9.19–11.24] | 89.04 | 0.70× |
| lscq | 5 | 7.23 ⚠ [7.04–11.87] | 138 | 0.45× |
| faaarray | 5 | 3.22 ⚠ [2.37–3.61] | 311 | 0.20× |
| orig_generic | 5 | 1.59 ⚠ [1.20–1.61] | 628 | 0.10× |
| boost_ms_queue | 5 | 1.54 ⚠ [1.34–1.60] | 650 | 0.10× |
| std_mutex | 5 | 1.47 ⚠ [1.13–1.50] | 680 | 0.09× |

## W9 split  E8  producers=8  consumers=8  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lcrq | 5 | 21.63 ⚠ [20.12–25.75] | 46.23 | 1.26× |
| shim_generic | 5 | 17.16 ⚠ [11.47–17.59] | 58.28 | 1.00× |
| moodycamel_cq_sp | 5 | 16.95 [15.96–17.18] | 59.00 | 0.99× |
| lprq | 5 | 14.39 ⚠ [11.94–20.16] | 69.51 | 0.84× |
| faaarray | 5 | 8.80 ⚠ [6.83–9.09] | 114 | 0.51× |
| lscq | 5 | 7.66 ⚠ [5.57–7.71] | 131 | 0.45× |
| orig_generic | 5 | 1.98 ⚠ [1.68–2.05] | 506 | 0.12× |
| std_mutex | 5 | 1.73 ⚠ [1.25–1.88] | 577 | 0.10× |
| boost_ms_queue | 5 | 1.56 ⚠ [1.35–1.69] | 641 | 0.09× |

## W9 split  E8  producers=8  consumers=8  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 16.77 ⚠ [12.18–17.55] | 59.63 | 1.00× |
| moodycamel_cq_sp | 5 | 12.31 ⚠ [10.55–12.51] | 81.23 | 0.73× |
| lcrq | 5 | 9.68 ⚠ [7.87–9.88] | 103 | 0.58× |
| lprq | 5 | 9.38 ⚠ [6.46–9.68] | 107 | 0.56× |
| lscq | 5 | 6.23 ⚠ [4.44–6.33] | 160 | 0.37× |
| faaarray | 5 | 3.30 ⚠ [2.82–4.01] | 303 | 0.20× |
| orig_generic | 5 | 1.86 ⚠ [1.47–1.87] | 539 | 0.11× |
| boost_ms_queue | 5 | 1.60 ⚠ [1.32–1.61] | 626 | 0.10× |
| std_mutex | 5 | 1.35 ⚠ [1.02–1.40] | 740 | 0.08× |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
