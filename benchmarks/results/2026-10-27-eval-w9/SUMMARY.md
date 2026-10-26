# eval-w9  (2026-10-26T18:23:57Z)

AMD EPYC 9115 16-Core Processor, kernel 6.8.0-137-generic, allowed cpus 16-31,48-63, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=0;`, git 

Control (shim_generic E64, median of 3 and 3): open 16.04, close 15.37 Mops/s, drift 4.2% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.001, rep 2 1.002, rep 3 1.002, rep 4 1.001 (n=441) — no trend.  Widest single row: lscq in W9 split  E8  producers=8  consumers=8  wait=spin  alloc=glibc, 69% across repetitions.

## W9 pairwise  E8  threads=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 62.21 [60.86–62.50] | 16.07 | 1.22× |
| boost_ms_queue | 5 | 59.17 [58.33–59.22] | 16.90 | 1.16× |
| std_mutex | 5 | 55.59 [54.65–56.38] | 17.99 | 1.09× |
| faaarray | 5 | 51.17 [50.60–51.98] | 19.54 | 1.00× |
| shim_generic | 5 | 51.07 [51.00–51.13] | 19.58 | 1.00× |
| lcrq | 5 | 49.14 [48.70–49.28] | 20.35 | 0.96× |
| lprq | 5 | 40.12 [39.50–40.16] | 24.92 | 0.79× |
| lscq | 5 | 32.13 [32.11–32.16] | 31.12 | 0.63× |
| orig_generic | 5 | 26.98 [26.16–26.99] | 37.06 | 0.53× |

## W9 pairwise  E8  threads=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 67.97 ⚠ [56.84–68.48] | 14.71 | 1.23× |
| boost_ms_queue | 5 | 64.23 ⚠ [54.52–64.31] | 15.57 | 1.16× |
| std_mutex | 5 | 63.70 ⚠ [53.30–64.00] | 15.70 | 1.15× |
| faaarray | 5 | 55.71 ⚠ [47.50–55.76] | 17.95 | 1.01× |
| shim_generic | 5 | 55.19 ⚠ [46.81–55.22] | 18.12 | 1.00× |
| lcrq | 5 | 52.34 ⚠ [45.82–53.14] | 19.11 | 0.95× |
| lprq | 5 | 43.17 ⚠ [38.04–43.72] | 23.17 | 0.78× |
| lscq | 5 | 33.58 ⚠ [29.27–33.61] | 29.78 | 0.61× |
| orig_generic | 5 | 28.83 ⚠ [24.89–28.85] | 34.69 | 0.52× |

## W9 pairwise  E8  threads=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 65.93 ⚠ [51.82–66.54] | 15.17 | 1.18× |
| boost_ms_queue | 5 | 62.10 ⚠ [53.49–62.78] | 16.10 | 1.11× |
| std_mutex | 5 | 61.25 ⚠ [51.52–62.06] | 16.33 | 1.10× |
| shim_generic | 5 | 55.92 ⚠ [47.56–55.94] | 17.88 | 1.00× |
| faaarray | 5 | 53.82 ⚠ [46.56–54.55] | 18.58 | 0.96× |
| lcrq | 5 | 52.05 ⚠ [44.88–52.11] | 19.21 | 0.93× |
| lprq | 5 | 42.86 ⚠ [37.17–42.89] | 23.33 | 0.77× |
| lscq | 5 | 33.17 ⚠ [29.11–33.21] | 30.14 | 0.59× |
| orig_generic | 5 | 29.05 ⚠ [25.25–29.10] | 34.42 | 0.52× |

## W9 pairwise  E8  threads=16  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lprq | 5 | 32.03 ⚠ [26.49–32.57] | 31.22 | 1.47× |
| lcrq | 5 | 31.09 ⚠ [26.14–31.75] | 32.16 | 1.42× |
| faaarray | 5 | 29.75 ⚠ [24.13–29.82] | 33.61 | 1.36× |
| lscq | 5 | 22.42 ⚠ [16.70–22.99] | 44.59 | 1.03× |
| shim_generic | 5 | 21.85 ⚠ [18.71–22.05] | 45.76 | 1.00× |
| moodycamel_cq_sp | 5 | 12.18 [12.15–12.32] | 82.08 | 0.56× |
| boost_ms_queue | 5 | 3.75 ⚠ [3.21–3.86] | 267 | 0.17× |
| orig_generic | 5 | 2.88 ⚠ [1.99–3.36] | 347 | 0.13× |
| std_mutex | 5 | 2.61 ⚠ [1.70–2.91] | 382 | 0.12× |

## W9 pairwise  E8  threads=16  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lcrq | 5 | 31.32 ⚠ [25.79–31.69] | 31.93 | 1.46× |
| faaarray | 5 | 29.68 ⚠ [23.73–30.63] | 33.69 | 1.39× |
| lprq | 5 | 29.40 ⚠ [23.23–31.43] | 34.02 | 1.37× |
| lscq | 5 | 22.14 [21.72–22.54] | 45.18 | 1.03× |
| shim_generic | 5 | 21.39 [21.05–21.48] | 46.74 | 1.00× |
| moodycamel_cq_sp | 5 | 14.90 [14.48–15.05] | 67.10 | 0.70× |
| boost_ms_queue | 5 | 3.57 ⚠ [3.12–3.64] | 280 | 0.17× |
| std_mutex | 5 | 3.41 ⚠ [1.65–3.63] | 293 | 0.16× |
| orig_generic | 5 | 3.36 ⚠ [3.17–3.48] | 298 | 0.16× |

## W9 pairwise  E8  threads=16  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lprq | 5 | 27.95 ⚠ [23.56–28.39] | 35.78 | 1.49× |
| lcrq | 5 | 27.43 ⚠ [23.91–28.11] | 36.45 | 1.46× |
| faaarray | 5 | 25.94 ⚠ [22.46–26.78] | 38.55 | 1.38× |
| lscq | 5 | 20.62 ⚠ [15.75–21.02] | 48.50 | 1.10× |
| shim_generic | 5 | 18.75 ⚠ [18.28–20.55] | 53.34 | 1.00× |
| moodycamel_cq_sp | 5 | 13.89 ⚠ [11.29–14.16] | 72.01 | 0.74× |
| boost_ms_queue | 5 | 3.30 ⚠ [2.81–3.37] | 303 | 0.18× |
| std_mutex | 5 | 2.54 ⚠ [2.48–2.70] | 394 | 0.14× |
| orig_generic | 5 | 1.62 ⚠ [1.53–2.30] | 616 | 0.09× |

## W9 pairwise  E8  threads=2  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 31.90 [31.00–32.51] | 31.35 | 1.29× |
| faaarray | 5 | 31.81 [30.89–32.33] | 31.43 | 1.29× |
| lcrq | 5 | 25.66 [25.34–25.86] | 38.97 | 1.04× |
| lprq | 5 | 24.65 [24.43–24.92] | 40.57 | 1.00× |
| shim_generic | 5 | 24.64 [24.42–25.28] | 40.59 | 1.00× |
| lscq | 5 | 17.15 [16.50–17.24] | 58.30 | 0.70× |
| orig_generic | 5 | 13.37 [13.08–13.88] | 74.79 | 0.54× |
| boost_ms_queue | 5 | 12.86 [12.59–13.05] | 77.75 | 0.52× |
| std_mutex | 5 | 11.40 ⚠ [10.03–12.61] | 87.73 | 0.46× |

## W9 pairwise  E8  threads=2  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 38.92 ⚠ [38.60–43.87] | 25.70 | 1.56× |
| faaarray | 5 | 28.77 ⚠ [26.70–31.75] | 34.76 | 1.15× |
| lcrq | 5 | 25.92 [25.30–26.66] | 38.58 | 1.04× |
| lprq | 5 | 25.71 [25.11–25.96] | 38.90 | 1.03× |
| shim_generic | 5 | 24.95 [24.66–25.59] | 40.09 | 1.00× |
| lscq | 5 | 17.29 [17.15–17.42] | 57.82 | 0.69× |
| orig_generic | 5 | 14.17 ⚠ [13.87–15.90] | 70.59 | 0.57× |
| boost_ms_queue | 5 | 12.87 [12.72–13.26] | 77.69 | 0.52× |
| std_mutex | 5 | 12.63 [12.37–12.86] | 79.19 | 0.51× |

## W9 pairwise  E8  threads=2  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| moodycamel_cq_sp | 5 | 38.50 ⚠ [37.86–44.66] | 25.98 | 1.52× |
| faaarray | 5 | 27.49 [27.31–28.28] | 36.37 | 1.09× |
| shim_generic | 5 | 25.31 [25.09–25.67] | 39.51 | 1.00× |
| lcrq | 5 | 23.97 [23.74–24.64] | 41.72 | 0.95× |
| lprq | 5 | 23.22 [22.94–23.48] | 43.06 | 0.92× |
| lscq | 5 | 16.80 [16.66–17.17] | 59.54 | 0.66× |
| orig_generic | 5 | 13.14 ⚠ [12.82–15.03] | 76.11 | 0.52× |
| std_mutex | 5 | 12.61 ⚠ [10.98–14.71] | 79.33 | 0.50× |
| boost_ms_queue | 5 | 12.40 [12.36–12.90] | 80.67 | 0.49× |

## W9 pairwise  E8  threads=4  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| faaarray | 5 | 37.34 [37.24–37.59] | 26.78 | 1.27× |
| lcrq | 5 | 36.26 [36.01–36.47] | 27.58 | 1.24× |
| lprq | 5 | 35.59 [35.13–36.23] | 28.10 | 1.21× |
| shim_generic | 5 | 29.34 [29.12–29.65] | 34.08 | 1.00× |
| lscq | 5 | 27.05 [26.86–27.08] | 36.97 | 0.92× |
| moodycamel_cq_sp | 5 | 23.65 [23.61–23.84] | 42.29 | 0.81× |
| std_mutex | 5 | 18.60 [17.90–19.04] | 53.77 | 0.63× |
| orig_generic | 5 | 11.26 [11.17–11.32] | 88.79 | 0.38× |
| boost_ms_queue | 5 | 9.69 ⚠ [9.10–9.95] | 103 | 0.33× |

## W9 pairwise  E8  threads=4  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| faaarray | 5 | 38.23 [38.07–38.31] | 26.16 | 1.28× |
| lprq | 5 | 37.01 [35.56–37.59] | 27.02 | 1.23× |
| lcrq | 5 | 36.93 [36.43–38.73] | 27.08 | 1.23× |
| shim_generic | 5 | 29.97 [29.80–30.52] | 33.37 | 1.00× |
| lscq | 5 | 26.51 [26.38–26.70] | 37.72 | 0.88× |
| moodycamel_cq_sp | 5 | 25.95 [25.70–26.01] | 38.53 | 0.87× |
| std_mutex | 5 | 20.74 ⚠ [19.57–21.79] | 48.21 | 0.69× |
| orig_generic | 5 | 11.97 [11.88–12.01] | 83.55 | 0.40× |
| boost_ms_queue | 5 | 9.68 ⚠ [9.01–9.95] | 103 | 0.32× |

## W9 pairwise  E8  threads=4  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| faaarray | 5 | 35.09 [35.00–35.25] | 28.50 | 1.19× |
| lcrq | 5 | 34.30 [33.98–34.39] | 29.16 | 1.17× |
| lprq | 5 | 33.16 [33.14–33.36] | 30.15 | 1.13× |
| shim_generic | 5 | 29.41 [29.15–29.85] | 34.00 | 1.00× |
| moodycamel_cq_sp | 5 | 25.15 [24.62–25.37] | 39.76 | 0.86× |
| lscq | 5 | 24.02 [23.97–24.33] | 41.63 | 0.82× |
| std_mutex | 5 | 18.03 ⚠ [16.94–19.01] | 55.46 | 0.61× |
| orig_generic | 5 | 10.12 [10.02–10.18] | 98.86 | 0.34× |
| boost_ms_queue | 5 | 9.51 [9.30–9.71] | 105 | 0.32× |

## W9 pairwise  E8  threads=8  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lcrq | 5 | 44.42 [44.23–45.10] | 22.51 | 1.42× |
| lprq | 5 | 44.38 [44.06–44.58] | 22.53 | 1.42× |
| faaarray | 5 | 43.59 [43.05–43.91] | 22.94 | 1.40× |
| lscq | 5 | 37.08 [36.92–37.48] | 26.97 | 1.19× |
| shim_generic | 5 | 31.18 [30.68–31.69] | 32.07 | 1.00× |
| moodycamel_cq_sp | 5 | 26.09 [25.87–26.54] | 38.34 | 0.84× |
| std_mutex | 5 | 8.35 [8.27–8.50] | 120 | 0.27× |
| orig_generic | 5 | 8.25 [8.15–8.51] | 121 | 0.26× |
| boost_ms_queue | 5 | 7.00 [6.93–7.09] | 143 | 0.22× |

## W9 pairwise  E8  threads=8  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| faaarray | 5 | 43.72 [42.47–44.01] | 22.87 | 1.42× |
| lcrq | 5 | 43.06 [42.78–44.06] | 23.22 | 1.40× |
| lprq | 5 | 42.70 [42.56–43.33] | 23.42 | 1.39× |
| lscq | 5 | 35.55 [35.28–36.13] | 28.13 | 1.16× |
| shim_generic | 5 | 30.69 [30.59–30.79] | 32.58 | 1.00× |
| moodycamel_cq_sp | 5 | 26.50 [25.08–26.90] | 37.74 | 0.86× |
| std_mutex | 5 | 8.68 [8.65–8.79] | 115 | 0.28× |
| orig_generic | 5 | 8.61 [8.55–8.71] | 116 | 0.28× |
| boost_ms_queue | 5 | 6.98 [6.95–7.12] | 143 | 0.23× |

## W9 pairwise  E8  threads=8  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lcrq | 5 | 40.31 [40.12–40.39] | 24.81 | 1.34× |
| lprq | 5 | 39.41 [39.16–39.50] | 25.37 | 1.31× |
| faaarray | 5 | 38.82 [38.63–38.99] | 25.76 | 1.29× |
| lscq | 5 | 33.03 [32.94–33.12] | 30.27 | 1.09× |
| shim_generic | 5 | 30.18 [29.92–30.23] | 33.14 | 1.00× |
| moodycamel_cq_sp | 5 | 25.42 [25.03–25.94] | 39.33 | 0.84× |
| std_mutex | 5 | 7.43 [7.36–7.55] | 135 | 0.25× |
| orig_generic | 5 | 7.15 [7.01–7.17] | 140 | 0.24× |
| boost_ms_queue | 5 | 6.56 [6.52–6.69] | 153 | 0.22× |

## W9 split  E8  producers=1  consumers=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 28.73 [28.43–28.88] | 34.81 | 1.93× |
| shim_mpsc | 5 | 27.72 [26.61–27.85] | 36.08 | 1.86× |
| shim_generic | 5 | 14.88 ⚠ [14.27–15.61] | 67.19 | 1.00× |
| lcrq | 5 | 9.68 ⚠ [7.22–9.87] | 103 | 0.65× |
| lprq | 5 | 8.04 [7.91–8.11] | 124 | 0.54× |
| moodycamel_cq_sp | 5 | 7.78 [7.67–7.79] | 129 | 0.52× |
| orig_mpsc | 5 | 7.02 [6.89–7.08] | 143 | 0.47× |
| faaarray | 5 | 6.37 [6.33–6.60] | 157 | 0.43× |
| lscq | 5 | 6.35 ⚠ [6.11–9.70] | 157 | 0.43× |
| boost_ms_queue | 5 | 5.71 [5.53–5.75] | 175 | 0.38× |
| std_mutex | 5 | 5.23 ⚠ [5.09–5.52] | 191 | 0.35× |
| orig_generic | 5 | 4.20 ⚠ [3.85–4.29] | 238 | 0.28× |

## W9 split  E8  producers=1  consumers=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 29.65 [29.05–29.68] | 33.72 | 2.00× |
| shim_mpsc | 5 | 29.27 [28.53–29.52] | 34.16 | 1.97× |
| orig_mpsc | 5 | 23.88 [23.52–24.03] | 41.88 | 1.61× |
| lcrq | 5 | 17.73 ⚠ [17.53–20.78] | 56.41 | 1.19× |
| shim_generic | 5 | 14.84 ⚠ [14.49–16.35] | 67.38 | 1.00× |
| lscq | 5 | 14.50 [14.31–14.57] | 68.98 | 0.98× |
| moodycamel_cq_sp | 5 | 14.25 [13.94–14.31] | 70.19 | 0.96× |
| lprq | 5 | 12.36 ⚠ [7.46–12.68] | 80.93 | 0.83× |
| std_mutex | 5 | 10.50 ⚠ [9.10–10.71] | 95.22 | 0.71× |
| faaarray | 5 | 9.60 [9.09–9.75] | 104 | 0.65× |
| boost_ms_queue | 5 | 6.84 [6.74–6.89] | 146 | 0.46× |
| orig_generic | 5 | 5.89 ⚠ [5.77–6.42] | 170 | 0.40× |

## W9 split  E8  producers=1  consumers=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 31.92 [31.86–31.96] | 31.32 | 2.26× |
| shim_mpsc | 5 | 29.41 [28.60–29.49] | 34.00 | 2.08× |
| shim_generic | 5 | 14.16 ⚠ [13.96–16.25] | 70.64 | 1.00× |
| moodycamel_cq_sp | 5 | 13.04 ⚠ [10.77–13.12] | 76.67 | 0.92× |
| std_mutex | 5 | 10.77 ⚠ [10.17–11.43] | 92.85 | 0.76× |
| lcrq | 5 | 10.01 ⚠ [8.31–10.13] | 99.90 | 0.71× |
| lscq | 5 | 8.97 ⚠ [6.92–9.06] | 112 | 0.63× |
| orig_mpsc | 5 | 8.48 ⚠ [8.17–13.76] | 118 | 0.60× |
| lprq | 5 | 8.42 [8.23–8.87] | 119 | 0.59× |
| faaarray | 5 | 7.58 [7.27–7.68] | 132 | 0.54× |
| boost_ms_queue | 5 | 5.95 [5.82–6.08] | 168 | 0.42× |
| orig_generic | 5 | 4.39 ⚠ [4.28–5.64] | 228 | 0.31× |

## W9 split  E8  producers=1  consumers=2  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 32.63 [31.46–32.76] | 30.64 | 3.09× |
| shim_generic | 5 | 10.56 [10.48–10.65] | 94.69 | 1.00× |
| moodycamel_cq_sp | 5 | 7.50 [7.47–7.55] | 133 | 0.71× |
| lcrq | 5 | 5.57 [5.31–5.82] | 179 | 0.53× |
| lprq | 5 | 5.06 [4.72–5.12] | 197 | 0.48× |
| boost_ms_queue | 5 | 4.72 [4.57–4.76] | 212 | 0.45× |
| orig_generic | 5 | 4.25 [4.08–4.29] | 235 | 0.40× |
| lscq | 5 | 4.12 ⚠ [3.65–4.16] | 243 | 0.39× |
| std_mutex | 5 | 3.52 [3.42–3.58] | 284 | 0.33× |
| faaarray | 5 | 2.74 [2.72–2.82] | 365 | 0.26× |

## W9 split  E8  producers=1  consumers=2  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 33.61 ⚠ [33.28–38.81] | 29.75 | 3.14× |
| moodycamel_cq_sp | 5 | 15.04 [14.97–15.14] | 66.49 | 1.41× |
| shim_generic | 5 | 10.70 [10.64–10.85] | 93.43 | 1.00× |
| lcrq | 5 | 7.14 ⚠ [7.00–7.98] | 140 | 0.67× |
| lprq | 5 | 6.11 [6.03–6.28] | 164 | 0.57× |
| orig_generic | 5 | 5.51 [5.33–5.72] | 181 | 0.52× |
| boost_ms_queue | 5 | 5.12 [5.04–5.39] | 195 | 0.48× |
| lscq | 5 | 4.75 ⚠ [4.37–4.84] | 211 | 0.44× |
| std_mutex | 5 | 4.23 [4.12–4.48] | 236 | 0.40× |
| faaarray | 5 | 3.31 ⚠ [3.12–3.47] | 302 | 0.31× |

## W9 split  E8  producers=1  consumers=2  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 34.39 [34.08–34.42] | 29.08 | 3.22× |
| shim_generic | 5 | 10.69 [10.62–10.80] | 93.53 | 1.00× |
| moodycamel_cq_sp | 5 | 10.57 ⚠ [9.24–10.84] | 94.59 | 0.99× |
| orig_generic | 5 | 5.78 [5.58–5.87] | 173 | 0.54× |
| lcrq | 5 | 5.25 [5.06–5.29] | 190 | 0.49× |
| lprq | 5 | 4.77 [4.67–4.92] | 209 | 0.45× |
| boost_ms_queue | 5 | 4.59 [4.52–4.69] | 218 | 0.43× |
| std_mutex | 5 | 3.90 [3.68–3.96] | 256 | 0.36× |
| lscq | 5 | 3.55 [3.50–3.58] | 282 | 0.33× |
| faaarray | 5 | 3.12 ⚠ [2.95–3.38] | 321 | 0.29× |

## W9 split  E8  producers=1  consumers=4  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 45.42 [44.81–45.49] | 22.02 | 4.99× |
| shim_generic | 5 | 9.11 [9.04–9.22] | 110 | 1.00× |
| moodycamel_cq_sp | 5 | 6.52 [6.48–6.60] | 153 | 0.72× |
| lcrq | 5 | 4.10 [3.85–4.15] | 244 | 0.45× |
| orig_generic | 5 | 3.88 [3.74–3.90] | 258 | 0.43× |
| lprq | 5 | 3.84 [3.79–3.97] | 261 | 0.42× |
| boost_ms_queue | 5 | 3.59 [3.50–3.62] | 278 | 0.39× |
| lscq | 5 | 2.47 ⚠ [2.25–2.49] | 406 | 0.27× |
| std_mutex | 5 | 1.77 [1.68–1.80] | 567 | 0.19× |
| faaarray | 5 | 1.11 ⚠ [1.08–1.34] | 897 | 0.12× |

## W9 split  E8  producers=1  consumers=4  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 45.58 [44.10–45.66] | 21.94 | 5.01× |
| moodycamel_cq_sp | 5 | 11.90 [11.72–11.93] | 84.03 | 1.31× |
| shim_generic | 5 | 9.10 [9.04–9.17] | 110 | 1.00× |
| orig_generic | 5 | 5.00 [4.98–5.05] | 200 | 0.55× |
| lcrq | 5 | 4.41 [4.37–4.62] | 227 | 0.48× |
| lprq | 5 | 4.20 [4.13–4.26] | 238 | 0.46× |
| boost_ms_queue | 5 | 3.93 [3.92–4.03] | 254 | 0.43× |
| lscq | 5 | 2.67 [2.48–2.68] | 375 | 0.29× |
| std_mutex | 5 | 1.98 [1.92–2.00] | 506 | 0.22× |
| faaarray | 5 | 1.18 ⚠ [1.11–1.58] | 851 | 0.13× |

## W9 split  E8  producers=1  consumers=4  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 45.54 [45.42–45.62] | 21.96 | 5.02× |
| shim_generic | 5 | 9.08 [8.86–9.16] | 110 | 1.00× |
| moodycamel_cq_sp | 5 | 8.02 ⚠ [7.91–8.81] | 125 | 0.88× |
| orig_generic | 5 | 5.01 [4.82–5.06] | 200 | 0.55× |
| lcrq | 5 | 3.67 [3.59–3.69] | 273 | 0.40× |
| boost_ms_queue | 5 | 3.53 [3.35–3.56] | 283 | 0.39× |
| lprq | 5 | 3.48 [3.41–3.50] | 288 | 0.38× |
| lscq | 5 | 2.25 [2.21–2.29] | 444 | 0.25× |
| std_mutex | 5 | 1.89 [1.85–1.89] | 530 | 0.21× |
| faaarray | 5 | 1.23 [1.21–1.24] | 814 | 0.14× |

## W9 split  E8  producers=1  consumers=8  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 29.12 ⚠ [27.63–30.36] | 34.35 | 13.89× |
| moodycamel_cq_sp | 5 | 2.86 [2.75–2.92] | 349 | 1.37× |
| orig_generic | 5 | 2.13 [2.07–2.15] | 469 | 1.02× |
| shim_generic | 5 | 2.10 ⚠ [1.60–2.23] | 477 | 1.00× |
| lcrq | 5 | 1.64 ⚠ [1.30–1.75] | 610 | 0.78× |
| lprq | 5 | 1.53 ⚠ [1.25–1.59] | 654 | 0.73× |
| boost_ms_queue | 5 | 1.31 ⚠ [1.05–1.36] | 762 | 0.63× |
| lscq | 5 | 0.91 [0.87–0.92] | 1,103 | 0.43× |
| std_mutex | 5 | 0.56 [0.55–0.59] | 1,780 | 0.27× |
| faaarray | 5 | 0.37 [0.36–0.37] | 2,735 | 0.17× |

## W9 split  E8  producers=1  consumers=8  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 30.11 ⚠ [26.95–30.56] | 33.21 | 13.96× |
| moodycamel_cq_sp | 5 | 3.79 [3.60–3.87] | 264 | 1.76× |
| orig_generic | 5 | 2.46 ⚠ [2.13–2.52] | 406 | 1.14× |
| shim_generic | 5 | 2.16 ⚠ [1.48–2.25] | 464 | 1.00× |
| lcrq | 5 | 1.64 ⚠ [1.25–1.75] | 610 | 0.76× |
| lprq | 5 | 1.54 ⚠ [1.28–1.61] | 648 | 0.72× |
| boost_ms_queue | 5 | 1.27 [1.26–1.33] | 787 | 0.59× |
| lscq | 5 | 0.95 ⚠ [0.87–0.96] | 1,054 | 0.44× |
| std_mutex | 5 | 0.55 ⚠ [0.43–0.60] | 1,810 | 0.26× |
| faaarray | 5 | 0.39 [0.38–0.39] | 2,575 | 0.18× |

## W9 split  E8  producers=1  consumers=8  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_spmc | 5 | 29.97 ⚠ [28.15–31.11] | 33.37 | 13.81× |
| moodycamel_cq_sp | 5 | 2.94 ⚠ [2.87–3.35] | 340 | 1.36× |
| orig_generic | 5 | 2.50 ⚠ [1.88–2.57] | 400 | 1.15× |
| shim_generic | 5 | 2.17 ⚠ [1.34–2.29] | 461 | 1.00× |
| lcrq | 5 | 1.51 ⚠ [1.17–1.55] | 661 | 0.70× |
| lprq | 5 | 1.44 ⚠ [1.12–1.49] | 694 | 0.66× |
| boost_ms_queue | 5 | 1.21 ⚠ [1.01–1.26] | 823 | 0.56× |
| lscq | 5 | 0.88 ⚠ [0.80–0.90] | 1,134 | 0.41× |
| std_mutex | 5 | 0.57 ⚠ [0.46–0.60] | 1,747 | 0.26× |
| faaarray | 5 | 0.38 ⚠ [0.35–0.47] | 2,651 | 0.17× |

## W9 split  E8  producers=2  consumers=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 27.80 [27.56–27.81] | 35.97 | 1.10× |
| shim_generic | 5 | 25.21 [24.87–25.26] | 39.67 | 1.00× |
| lcrq | 5 | 14.01 [13.86–14.43] | 71.38 | 0.56× |
| lprq | 5 | 13.77 [13.10–14.14] | 72.60 | 0.55× |
| faaarray | 5 | 12.02 [11.94–12.43] | 83.23 | 0.48× |
| lscq | 5 | 11.03 [10.86–11.13] | 90.64 | 0.44× |
| orig_mpsc | 5 | 10.63 ⚠ [10.04–11.23] | 94.04 | 0.42× |
| moodycamel_cq_sp | 5 | 10.53 [10.42–10.58] | 94.93 | 0.42× |
| boost_ms_queue | 5 | 7.40 [7.09–7.44] | 135 | 0.29× |
| std_mutex | 5 | 6.17 [6.12–6.19] | 162 | 0.24× |
| orig_generic | 5 | 4.52 [4.41–4.56] | 221 | 0.18× |

## W9 split  E8  producers=2  consumers=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 29.86 ⚠ [23.96–29.91] | 33.49 | 1.10× |
| shim_generic | 5 | 27.14 ⚠ [23.37–27.17] | 36.84 | 1.00× |
| orig_mpsc | 5 | 23.14 ⚠ [20.08–24.03] | 43.22 | 0.85× |
| faaarray | 5 | 18.66 [17.91–18.99] | 53.59 | 0.69× |
| lcrq | 5 | 18.48 ⚠ [17.63–19.95] | 54.13 | 0.68× |
| lprq | 5 | 16.76 [16.64–17.55] | 59.66 | 0.62× |
| moodycamel_cq_sp | 5 | 14.99 ⚠ [13.68–15.10] | 66.73 | 0.55× |
| lscq | 5 | 13.57 [13.47–14.00] | 73.68 | 0.50× |
| std_mutex | 5 | 8.07 [7.73–8.23] | 124 | 0.30× |
| boost_ms_queue | 5 | 8.00 [7.73–8.06] | 125 | 0.29× |
| orig_generic | 5 | 5.33 [5.15–5.45] | 188 | 0.20× |

## W9 split  E8  producers=2  consumers=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 31.52 [30.81–31.88] | 31.73 | 1.15× |
| shim_generic | 5 | 27.31 [26.96–27.36] | 36.62 | 1.00× |
| orig_mpsc | 5 | 18.76 [18.57–18.95] | 53.32 | 0.69× |
| lcrq | 5 | 14.75 ⚠ [13.97–15.88] | 67.79 | 0.54× |
| moodycamel_cq_sp | 5 | 13.57 ⚠ [13.01–14.10] | 73.67 | 0.50× |
| lprq | 5 | 13.44 ⚠ [12.49–13.65] | 74.41 | 0.49× |
| faaarray | 5 | 12.88 ⚠ [12.03–13.43] | 77.63 | 0.47× |
| lscq | 5 | 9.87 [9.67–9.96] | 101 | 0.36× |
| std_mutex | 5 | 7.90 [7.79–8.20] | 127 | 0.29× |
| boost_ms_queue | 5 | 7.41 [7.24–7.48] | 135 | 0.27× |
| orig_generic | 5 | 5.41 [5.38–5.49] | 185 | 0.20× |

## W9 split  E8  producers=2  consumers=2  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 22.53 [22.14–22.98] | 44.39 | 1.00× |
| lcrq | 5 | 12.23 [12.07–12.48] | 81.75 | 0.54× |
| lprq | 5 | 10.71 [10.54–10.89] | 93.35 | 0.48× |
| moodycamel_cq_sp | 5 | 9.57 [9.55–9.65] | 104 | 0.42× |
| faaarray | 5 | 7.59 ⚠ [6.89–7.84] | 132 | 0.34× |
| lscq | 5 | 7.46 [7.34–7.61] | 134 | 0.33× |
| boost_ms_queue | 5 | 5.76 [5.55–5.98] | 174 | 0.26× |
| std_mutex | 5 | 4.40 [4.38–4.58] | 227 | 0.20× |
| orig_generic | 5 | 3.90 [3.85–3.91] | 256 | 0.17× |

## W9 split  E8  producers=2  consumers=2  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 22.45 [22.18–22.70] | 44.54 | 1.00× |
| lcrq | 5 | 13.81 [13.31–14.11] | 72.41 | 0.62× |
| moodycamel_cq_sp | 5 | 13.73 [13.65–13.78] | 72.83 | 0.61× |
| lprq | 5 | 12.64 [12.46–12.77] | 79.13 | 0.56× |
| faaarray | 5 | 11.32 [10.82–11.63] | 88.34 | 0.50× |
| lscq | 5 | 10.45 ⚠ [9.67–11.43] | 95.71 | 0.47× |
| boost_ms_queue | 5 | 6.44 [6.22–6.49] | 155 | 0.29× |
| std_mutex | 5 | 5.61 [5.38–5.72] | 178 | 0.25× |
| orig_generic | 5 | 4.80 [4.78–4.81] | 208 | 0.21× |

## W9 split  E8  producers=2  consumers=2  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 22.63 [22.32–22.73] | 44.18 | 1.00× |
| moodycamel_cq_sp | 5 | 14.87 ⚠ [11.92–15.06] | 67.25 | 0.66× |
| lcrq | 5 | 9.86 [9.76–9.97] | 101 | 0.44× |
| lprq | 5 | 9.06 [8.89–9.15] | 110 | 0.40× |
| faaarray | 5 | 7.93 ⚠ [7.59–8.34] | 126 | 0.35× |
| lscq | 5 | 6.08 [6.06–6.14] | 164 | 0.27× |
| boost_ms_queue | 5 | 5.96 [5.93–5.98] | 168 | 0.26× |
| std_mutex | 5 | 5.61 [5.50–5.67] | 178 | 0.25× |
| orig_generic | 5 | 4.75 [4.71–4.78] | 211 | 0.21× |

## W9 split  E8  producers=4  consumers=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 27.87 [26.48–27.91] | 35.88 | 1.12× |
| shim_generic | 5 | 24.93 [24.24–24.95] | 40.11 | 1.00× |
| lcrq | 5 | 14.13 [13.75–14.16] | 70.78 | 0.57× |
| lscq | 5 | 13.82 [13.46–14.04] | 72.34 | 0.55× |
| lprq | 5 | 13.59 [13.44–13.64] | 73.57 | 0.55× |
| moodycamel_cq_sp | 5 | 13.56 [13.53–13.66] | 73.73 | 0.54× |
| faaarray | 5 | 12.77 [12.68–12.79] | 78.30 | 0.51× |
| orig_mpsc | 5 | 10.55 [10.53–10.75] | 94.76 | 0.42× |
| boost_ms_queue | 5 | 6.90 ⚠ [6.43–6.91] | 145 | 0.28× |
| std_mutex | 5 | 5.43 [5.40–5.47] | 184 | 0.22× |
| orig_generic | 5 | 4.12 [4.10–4.13] | 243 | 0.17× |

## W9 split  E8  producers=4  consumers=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 29.50 [29.20–29.69] | 33.90 | 1.09× |
| shim_generic | 5 | 27.02 [26.91–27.09] | 37.00 | 1.00× |
| orig_mpsc | 5 | 22.48 [22.39–22.78] | 44.48 | 0.83× |
| lcrq | 5 | 20.26 [20.14–20.31] | 49.35 | 0.75× |
| lprq | 5 | 20.13 [19.95–20.96] | 49.67 | 0.75× |
| faaarray | 5 | 18.80 [18.67–19.09] | 53.18 | 0.70× |
| lscq | 5 | 16.87 [16.76–16.93] | 59.28 | 0.62× |
| moodycamel_cq_sp | 5 | 16.18 [16.11–16.27] | 61.82 | 0.60× |
| boost_ms_queue | 5 | 7.07 [7.03–7.25] | 141 | 0.26× |
| std_mutex | 5 | 6.21 [6.17–6.24] | 161 | 0.23× |
| orig_generic | 5 | 4.69 [4.65–4.77] | 213 | 0.17× |

## W9 split  E8  producers=4  consumers=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 31.72 [31.16–31.83] | 31.52 | 1.15× |
| shim_generic | 5 | 27.56 [27.20–27.59] | 36.28 | 1.00× |
| orig_mpsc | 5 | 19.50 [19.19–19.61] | 51.28 | 0.71× |
| moodycamel_cq_sp | 5 | 14.64 [14.40–14.70] | 68.32 | 0.53× |
| faaarray | 5 | 13.84 [13.73–13.91] | 72.28 | 0.50× |
| lcrq | 5 | 13.72 [13.57–13.74] | 72.87 | 0.50× |
| lprq | 5 | 13.41 [13.33–13.53] | 74.55 | 0.49× |
| lscq | 5 | 12.94 [12.18–13.16] | 77.28 | 0.47× |
| boost_ms_queue | 5 | 7.04 [7.03–7.12] | 142 | 0.26× |
| std_mutex | 5 | 6.07 ⚠ [5.57–6.18] | 165 | 0.22× |
| orig_generic | 5 | 4.75 [4.73–4.77] | 211 | 0.17× |

## W9 split  E8  producers=4  consumers=4  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 26.04 [25.94–26.21] | 38.40 | 1.00× |
| lcrq | 5 | 16.20 ⚠ [14.43–16.51] | 61.75 | 0.62× |
| lprq | 5 | 14.85 ⚠ [13.92–15.50] | 67.34 | 0.57× |
| moodycamel_cq_sp | 5 | 12.70 [12.68–12.77] | 78.75 | 0.49× |
| lscq | 5 | 10.66 [10.57–10.76] | 93.78 | 0.41× |
| faaarray | 5 | 7.53 ⚠ [6.98–7.81] | 133 | 0.29× |
| boost_ms_queue | 5 | 4.18 [4.05–4.24] | 239 | 0.16× |
| orig_generic | 5 | 3.38 [3.23–3.40] | 296 | 0.13× |
| std_mutex | 5 | 3.22 [3.11–3.24] | 310 | 0.12× |

## W9 split  E8  producers=4  consumers=4  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 46.67 [45.55–46.96] | 21.43 | 1.00× |
| moodycamel_cq_sp | 5 | 17.48 [17.37–18.12] | 57.20 | 0.37× |
| lcrq | 5 | 17.13 ⚠ [15.92–18.12] | 58.38 | 0.37× |
| lprq | 5 | 15.23 ⚠ [14.23–15.45] | 65.66 | 0.33× |
| lscq | 5 | 12.04 [11.64–12.39] | 83.04 | 0.26× |
| faaarray | 5 | 10.83 ⚠ [10.45–11.93] | 92.34 | 0.23× |
| boost_ms_queue | 5 | 4.18 [4.17–4.35] | 239 | 0.09× |
| orig_generic | 5 | 3.93 [3.89–3.93] | 255 | 0.08× |
| std_mutex | 5 | 3.52 [3.48–3.54] | 284 | 0.08× |

## W9 split  E8  producers=4  consumers=4  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 46.89 [45.94–47.03] | 21.33 | 1.00× |
| moodycamel_cq_sp | 5 | 16.67 ⚠ [15.82–17.47] | 59.99 | 0.36× |
| lcrq | 5 | 12.00 [11.83–12.30] | 83.30 | 0.26× |
| lprq | 5 | 11.60 [11.29–11.92] | 86.24 | 0.25× |
| lscq | 5 | 8.48 [8.34–8.53] | 118 | 0.18× |
| faaarray | 5 | 6.81 [6.71–6.98] | 147 | 0.15× |
| boost_ms_queue | 5 | 4.10 [4.08–4.14] | 244 | 0.09× |
| orig_generic | 5 | 3.82 [3.77–3.86] | 262 | 0.08× |
| std_mutex | 5 | 3.39 [3.39–3.44] | 295 | 0.07× |

## W9 split  E8  producers=8  consumers=1  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 25.57 ⚠ [18.94–25.64] | 39.11 | 1.07× |
| shim_generic | 5 | 23.95 [23.84–24.13] | 41.76 | 1.00× |
| lprq | 5 | 10.01 [9.89–10.06] | 99.87 | 0.42× |
| lcrq | 5 | 9.99 [9.84–10.03] | 100 | 0.42× |
| faaarray | 5 | 9.87 [9.73–9.99] | 101 | 0.41× |
| moodycamel_cq_sp | 5 | 8.82 ⚠ [6.86–8.91] | 113 | 0.37× |
| lscq | 5 | 7.48 [7.32–7.75] | 134 | 0.31× |
| orig_mpsc | 5 | 3.84 ⚠ [3.45–3.87] | 261 | 0.16× |
| std_mutex | 5 | 2.57 ⚠ [2.42–2.65] | 389 | 0.11× |
| boost_ms_queue | 5 | 2.01 ⚠ [1.84–2.04] | 498 | 0.08× |
| orig_generic | 5 | 1.77 [1.72–1.80] | 565 | 0.07× |

## W9 split  E8  producers=8  consumers=1  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 25.99 ⚠ [13.49–26.11] | 38.47 | 1.00× |
| shim_mpsc | 5 | 25.55 ⚠ [20.53–25.69] | 39.14 | 0.98× |
| lprq | 5 | 14.02 [13.69–14.19] | 71.35 | 0.54× |
| lcrq | 5 | 13.70 [13.10–13.98] | 73.01 | 0.53× |
| orig_mpsc | 5 | 13.51 ⚠ [8.36–13.74] | 74.02 | 0.52× |
| moodycamel_cq_sp | 5 | 13.50 [12.61–13.69] | 74.09 | 0.52× |
| faaarray | 5 | 13.23 ⚠ [11.42–13.28] | 75.56 | 0.51× |
| lscq | 5 | 12.25 [11.97–12.42] | 81.65 | 0.47× |
| std_mutex | 5 | 3.57 [3.51–3.73] | 280 | 0.14× |
| orig_generic | 5 | 2.74 ⚠ [2.51–2.83] | 365 | 0.11× |
| boost_ms_queue | 5 | 2.36 ⚠ [1.51–2.42] | 424 | 0.09× |

## W9 split  E8  producers=8  consumers=1  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_mpsc | 5 | 25.21 ⚠ [19.45–25.49] | 39.67 | 1.90× |
| shim_generic | 5 | 13.28 ⚠ [12.02–25.36] | 75.33 | 1.00× |
| moodycamel_cq_sp | 5 | 9.99 ⚠ [8.15–10.32] | 100 | 0.75× |
| orig_mpsc | 5 | 9.76 ⚠ [9.12–10.18] | 102 | 0.74× |
| lprq | 5 | 9.42 [9.24–9.50] | 106 | 0.71× |
| faaarray | 5 | 9.38 ⚠ [8.84–9.77] | 107 | 0.71× |
| lcrq | 5 | 9.33 ⚠ [8.03–9.59] | 107 | 0.70× |
| lscq | 5 | 7.69 ⚠ [6.72–7.87] | 130 | 0.58× |
| std_mutex | 5 | 2.81 ⚠ [2.00–2.82] | 356 | 0.21× |
| orig_generic | 5 | 2.21 ⚠ [1.75–2.26] | 452 | 0.17× |
| boost_ms_queue | 5 | 2.21 ⚠ [1.72–2.26] | 453 | 0.17× |

## W9 split  E8  producers=8  consumers=8  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 16.86 ⚠ [15.30–16.97] | 59.30 | 1.00× |
| lcrq | 5 | 15.33 ⚠ [14.52–15.92] | 65.25 | 0.91× |
| lprq | 5 | 14.30 ⚠ [12.73–15.50] | 69.93 | 0.85× |
| moodycamel_cq_sp | 5 | 10.34 ⚠ [8.88–11.00] | 96.71 | 0.61× |
| lscq | 5 | 7.51 ⚠ [6.82–11.54] | 133 | 0.45× |
| faaarray | 5 | 3.46 ⚠ [2.48–3.65] | 289 | 0.20× |
| orig_generic | 5 | 1.56 [1.49–1.60] | 642 | 0.09× |
| boost_ms_queue | 5 | 1.53 ⚠ [1.50–1.65] | 652 | 0.09× |
| std_mutex | 5 | 1.38 ⚠ [0.99–1.46] | 727 | 0.08× |

## W9 split  E8  producers=8  consumers=8  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| lcrq | 5 | 23.34 ⚠ [16.94–25.02] | 42.84 | 1.38× |
| shim_generic | 5 | 16.87 ⚠ [11.36–17.53] | 59.29 | 1.00× |
| moodycamel_cq_sp | 5 | 16.62 ⚠ [14.31–17.39] | 60.18 | 0.99× |
| lprq | 5 | 12.29 ⚠ [12.06–13.31] | 81.39 | 0.73× |
| faaarray | 5 | 8.95 ⚠ [6.07–12.19] | 112 | 0.53× |
| lscq | 5 | 7.42 ⚠ [5.51–7.74] | 135 | 0.44× |
| orig_generic | 5 | 2.04 ⚠ [1.54–2.07] | 491 | 0.12× |
| std_mutex | 5 | 1.72 ⚠ [1.33–1.94] | 581 | 0.10× |
| boost_ms_queue | 5 | 1.56 ⚠ [1.47–1.65] | 640 | 0.09× |

## W9 split  E8  producers=8  consumers=8  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op | Mops/s vs shim_generic |
|---|---|---|---|---|
| shim_generic | 5 | 16.69 ⚠ [11.92–17.40] | 59.91 | 1.00× |
| moodycamel_cq_sp | 5 | 12.05 ⚠ [10.41–12.38] | 82.97 | 0.72× |
| lcrq | 5 | 10.28 ⚠ [9.95–14.87] | 97.32 | 0.62× |
| lprq | 5 | 9.48 ⚠ [6.28–9.61] | 105 | 0.57× |
| lscq | 5 | 5.07 ⚠ [4.27–6.13] | 197 | 0.30× |
| faaarray | 5 | 3.59 ⚠ [3.15–4.17] | 279 | 0.21× |
| orig_generic | 5 | 1.67 ⚠ [1.27–1.86] | 599 | 0.10× |
| boost_ms_queue | 5 | 1.55 ⚠ [1.31–1.61] | 646 | 0.09× |
| std_mutex | 5 | 1.27 ⚠ [0.93–1.41] | 790 | 0.08× |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
