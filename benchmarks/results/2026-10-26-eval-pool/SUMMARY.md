# eval-pool  (2026-10-26T10:11:28Z)

AMD EPYC 9115 16-Core Processor, kernel 6.8.0-137-generic, allowed cpus 16-31,48-63, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=0;`, git 

Control (shim_generic E64, median of 3 and 3): open 15.63, close 15.14 Mops/s, drift 3.1% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.000, rep 2 1.000, rep 3 1.001, rep 4 1.001 (n=432) — no trend.  Widest single row: cell_mpmc in W11  E16  producers=1  consumers=4  work=100  alloc=glibc, 148% across repetitions.

## W11  E16  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 136 [135–136] | 7.37 | 9.15× |
| heap_spmc | 5 | 121 [121–121] | 8.28 | 8.15× |
| cell_mpmc | 5 | 15.20 [14.99–15.39] | 65.80 | 1.03× |
| heap_mpmc | 5 | 14.82 [14.70–14.92] | 67.48 | 1.00× |

## W11  E16  producers=1  consumers=1  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 136 [135–136] | 7.38 | 9.07× |
| heap_spmc | 5 | 121 [120–121] | 8.28 | 8.09× |
| heap_mpmc | 5 | 14.94 [14.81–14.99] | 66.94 | 1.00× |
| cell_mpmc | 5 | 14.83 [14.82–15.26] | 67.43 | 0.99× |

## W11  E16  producers=1  consumers=1  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 136 [135–136] | 7.38 | 8.93× |
| heap_spmc | 5 | 121 [120–121] | 8.28 | 7.96× |
| heap_mpmc | 5 | 15.17 [14.96–15.28] | 65.91 | 1.00× |
| cell_mpmc | 5 | 14.80 [14.62–15.36] | 67.57 | 0.98× |

## W11  E16  producers=1  consumers=1  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.04 [12.98–13.07] | 76.69 | 1.07× |
| cell_mpmc | 5 | 12.75 [12.44–12.75] | 78.46 | 1.05× |
| heap_spmc | 5 | 12.46 [12.41–12.47] | 80.27 | 1.03× |
| heap_mpmc | 5 | 12.14 [12.01–12.15] | 82.38 | 1.00× |

## W11  E16  producers=1  consumers=1  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.04 [13.01–13.06] | 76.68 | 1.07× |
| cell_mpmc | 5 | 12.72 [12.48–12.73] | 78.59 | 1.05× |
| heap_spmc | 5 | 12.46 [12.44–12.47] | 80.25 | 1.03× |
| heap_mpmc | 5 | 12.14 [11.96–12.14] | 82.40 | 1.00× |

## W11  E16  producers=1  consumers=1  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.05 [12.82–13.06] | 76.64 | 1.08× |
| cell_mpmc | 5 | 12.70 [12.68–12.72] | 78.72 | 1.05× |
| heap_spmc | 5 | 12.46 [12.44–12.48] | 80.29 | 1.03× |
| heap_mpmc | 5 | 12.13 [12.11–12.14] | 82.46 | 1.00× |

## W11  E16  producers=1  consumers=1  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 1.02 [1.02–1.02] | 976 | 1.00× |
| cell_spmc | 5 | 1.02 [1.02–1.02] | 977 | 1.00× |
| heap_spmc | 5 | 1.02 [1.02–1.02] | 979 | 1.00× |
| heap_mpmc | 5 | 1.02 [1.02–1.02] | 980 | 1.00× |

## W11  E16  producers=1  consumers=1  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 1.02 [1.02–1.02] | 976 | 1.00× |
| cell_spmc | 5 | 1.02 [1.02–1.03] | 976 | 1.00× |
| heap_spmc | 5 | 1.02 [1.01–1.02] | 979 | 1.00× |
| heap_mpmc | 5 | 1.02 [1.02–1.02] | 980 | 1.00× |

## W11  E16  producers=1  consumers=1  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.02 [1.02–1.03] | 976 | 1.00× |
| cell_mpmc | 5 | 1.02 [1.02–1.03] | 977 | 1.00× |
| heap_spmc | 5 | 1.02 [1.02–1.02] | 978 | 1.00× |
| heap_mpmc | 5 | 1.02 [1.02–1.02] | 980 | 1.00× |

## W11  E16  producers=1  consumers=2  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 31.91 ⚠ [27.34–33.71] | 31.34 | 2.55× |
| heap_spmc | 5 | 24.61 [23.81–25.21] | 40.64 | 1.96× |
| heap_mpmc | 5 | 12.53 ⚠ [12.46–26.98] | 79.82 | 1.00× |
| cell_mpmc | 5 | 11.87 [11.71–12.01] | 84.24 | 0.95× |

## W11  E16  producers=1  consumers=2  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 29.05 ⚠ [27.00–33.06] | 34.42 | 2.34× |
| heap_spmc | 5 | 24.27 ⚠ [22.37–25.96] | 41.21 | 1.96× |
| heap_mpmc | 5 | 12.40 ⚠ [12.37–29.18] | 80.65 | 1.00× |
| cell_mpmc | 5 | 12.00 [11.93–12.02] | 83.35 | 0.97× |

## W11  E16  producers=1  consumers=2  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 30.75 ⚠ [28.31–35.33] | 32.52 | 2.46× |
| heap_spmc | 5 | 23.68 ⚠ [20.82–24.37] | 42.22 | 1.89× |
| heap_mpmc | 5 | 12.50 [12.31–12.53] | 79.97 | 1.00× |
| cell_mpmc | 5 | 12.21 [12.20–12.25] | 81.88 | 0.98× |

## W11  E16  producers=1  consumers=2  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 16.41 [16.39–16.42] | 60.96 | 1.12× |
| cell_spmc | 5 | 15.56 [15.33–15.75] | 64.27 | 1.06× |
| heap_mpmc | 5 | 14.67 [14.56–14.75] | 68.18 | 1.00× |
| heap_spmc | 5 | 14.16 [13.79–14.42] | 70.62 | 0.97× |

## W11  E16  producers=1  consumers=2  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 16.39 [16.30–16.40] | 61.02 | 1.12× |
| cell_spmc | 5 | 15.57 [15.33–15.72] | 64.24 | 1.06× |
| heap_mpmc | 5 | 14.63 [14.40–14.68] | 68.33 | 1.00× |
| heap_spmc | 5 | 14.28 [14.08–14.55] | 70.05 | 0.98× |

## W11  E16  producers=1  consumers=2  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 16.42 [16.16–16.43] | 60.91 | 1.11× |
| cell_spmc | 5 | 16.00 [15.68–16.17] | 62.50 | 1.09× |
| heap_mpmc | 5 | 14.73 [14.44–14.75] | 67.91 | 1.00× |
| heap_spmc | 5 | 14.36 [13.89–14.50] | 69.62 | 0.98× |

## W11  E16  producers=1  consumers=2  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.99 [1.99–1.99] | 502 | 1.03× |
| cell_mpmc | 5 | 1.98 [1.98–1.98] | 504 | 1.02× |
| heap_spmc | 5 | 1.94 [1.94–1.94] | 515 | 1.00× |
| heap_mpmc | 5 | 1.94 [1.93–1.94] | 516 | 1.00× |

## W11  E16  producers=1  consumers=2  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.99 [1.99–1.99] | 502 | 1.03× |
| cell_mpmc | 5 | 1.98 [1.98–1.99] | 504 | 1.03× |
| heap_spmc | 5 | 1.94 [1.94–1.95] | 515 | 1.00× |
| heap_mpmc | 5 | 1.94 [1.93–1.94] | 517 | 1.00× |

## W11  E16  producers=1  consumers=2  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.99 [1.99–2.00] | 502 | 1.03× |
| cell_mpmc | 5 | 1.99 [1.98–1.99] | 503 | 1.03× |
| heap_spmc | 5 | 1.94 [1.94–1.95] | 515 | 1.00× |
| heap_mpmc | 5 | 1.93 [1.93–1.94] | 517 | 1.00× |

## W11  E16  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 19.24 ⚠ [17.97–20.17] | 51.98 | 1.91× |
| heap_spmc | 5 | 17.20 [16.97–17.40] | 58.15 | 1.71× |
| heap_mpmc | 5 | 10.08 [10.05–10.11] | 99.23 | 1.00× |
| cell_mpmc | 5 | 9.96 [9.93–9.99] | 100 | 0.99× |

## W11  E16  producers=1  consumers=4  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 19.52 [18.45–19.54] | 51.24 | 1.94× |
| heap_spmc | 5 | 16.77 ⚠ [15.45–17.69] | 59.64 | 1.66× |
| heap_mpmc | 5 | 10.07 [10.04–10.10] | 99.29 | 1.00× |
| cell_mpmc | 5 | 9.95 [9.93–9.96] | 101 | 0.99× |

## W11  E16  producers=1  consumers=4  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 19.59 ⚠ [18.30–20.50] | 51.06 | 1.92× |
| heap_spmc | 5 | 17.38 [16.56–17.80] | 57.55 | 1.70× |
| heap_mpmc | 5 | 10.19 [10.15–10.24] | 98.10 | 1.00× |
| cell_mpmc | 5 | 10.07 [10.05–10.12] | 99.30 | 0.99× |

## W11  E16  producers=1  consumers=4  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 16.40 ⚠ [10.59–26.27] | 60.98 | 1.48× |
| heap_mpmc | 5 | 11.07 [11.04–11.11] | 90.31 | 1.00× |
| cell_spmc | 5 | 9.67 [9.08–9.71] | 103 | 0.87× |
| heap_spmc | 5 | 8.71 [8.48–8.78] | 115 | 0.79× |

## W11  E16  producers=1  consumers=4  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 22.95 ⚠ [10.58–26.56] | 43.57 | 2.08× |
| heap_mpmc | 5 | 11.04 [11.01–11.11] | 90.62 | 1.00× |
| cell_spmc | 5 | 9.69 [9.43–9.82] | 103 | 0.88× |
| heap_spmc | 5 | 8.53 [8.47–8.62] | 117 | 0.77× |

## W11  E16  producers=1  consumers=4  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 11.16 ⚠ [11.12–16.31] | 89.62 | 1.00× |
| cell_mpmc | 5 | 10.64 [10.58–10.67] | 93.95 | 0.95× |
| cell_spmc | 5 | 9.56 [9.00–9.66] | 105 | 0.86× |
| heap_spmc | 5 | 8.68 [8.61–8.77] | 115 | 0.78× |

## W11  E16  producers=1  consumers=4  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.98 [3.97–3.98] | 251 | 1.03× |
| cell_mpmc | 5 | 3.94 [3.94–3.94] | 254 | 1.02× |
| heap_spmc | 5 | 3.88 [3.88–3.90] | 257 | 1.01× |
| heap_mpmc | 5 | 3.84 [3.84–3.85] | 260 | 1.00× |

## W11  E16  producers=1  consumers=4  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.98 [3.97–3.98] | 251 | 1.03× |
| cell_mpmc | 5 | 3.94 [3.94–3.94] | 254 | 1.02× |
| heap_spmc | 5 | 3.89 [3.87–3.90] | 257 | 1.01× |
| heap_mpmc | 5 | 3.85 [3.84–3.85] | 260 | 1.00× |

## W11  E16  producers=1  consumers=4  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.97 [3.97–3.98] | 252 | 1.03× |
| cell_mpmc | 5 | 3.94 [3.94–3.94] | 254 | 1.02× |
| heap_spmc | 5 | 3.89 [3.88–3.89] | 257 | 1.01× |
| heap_mpmc | 5 | 3.85 [3.84–3.85] | 260 | 1.00× |

## W11  E16  producers=1  consumers=8  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 9.22 ⚠ [8.88–12.67] | 108 | 3.63× |
| heap_spmc | 5 | 7.29 ⚠ [7.16–8.38] | 137 | 2.87× |
| heap_mpmc | 5 | 2.54 [2.53–2.71] | 393 | 1.00× |
| cell_mpmc | 5 | 2.45 ⚠ [1.87–2.49] | 408 | 0.96× |

## W11  E16  producers=1  consumers=8  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 9.11 [8.85–9.45] | 110 | 3.61× |
| heap_spmc | 5 | 7.46 ⚠ [6.76–8.91] | 134 | 2.96× |
| heap_mpmc | 5 | 2.52 [2.49–2.63] | 397 | 1.00× |
| cell_mpmc | 5 | 2.46 [2.38–2.49] | 407 | 0.97× |

## W11  E16  producers=1  consumers=8  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 9.36 ⚠ [8.64–13.68] | 107 | 3.62× |
| heap_spmc | 5 | 7.52 ⚠ [7.00–8.32] | 133 | 2.91× |
| heap_mpmc | 5 | 2.59 ⚠ [1.70–2.75] | 387 | 1.00× |
| cell_mpmc | 5 | 2.45 ⚠ [1.71–2.52] | 408 | 0.95× |

## W11  E16  producers=1  consumers=8  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.42 [5.17–5.48] | 184 | 2.09× |
| heap_spmc | 5 | 4.59 ⚠ [3.04–4.66] | 218 | 1.77× |
| heap_mpmc | 5 | 2.59 ⚠ [1.75–2.64] | 386 | 1.00× |
| cell_mpmc | 5 | 2.56 [2.43–2.57] | 391 | 0.99× |

## W11  E16  producers=1  consumers=8  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.26 [5.11–5.55] | 190 | 1.99× |
| heap_spmc | 5 | 4.65 [4.46–4.70] | 215 | 1.76× |
| heap_mpmc | 5 | 2.65 [2.52–2.75] | 378 | 1.00× |
| cell_mpmc | 5 | 2.51 [2.41–2.53] | 399 | 0.95× |

## W11  E16  producers=1  consumers=8  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.33 ⚠ [4.54–5.42] | 187 | 2.01× |
| heap_spmc | 5 | 4.58 ⚠ [2.98–4.76] | 219 | 1.72× |
| heap_mpmc | 5 | 2.66 ⚠ [1.79–2.79] | 377 | 1.00× |
| cell_mpmc | 5 | 2.42 ⚠ [1.80–2.55] | 413 | 0.91× |

## W11  E16  producers=1  consumers=8  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 7.19 [6.66–7.20] | 139 | 1.02× |
| heap_mpmc | 5 | 7.03 [6.99–7.04] | 142 | 1.00× |
| cell_spmc | 5 | 6.88 ⚠ [3.92–7.10] | 145 | 0.98× |
| heap_spmc | 5 | 4.07 [4.02–4.10] | 246 | 0.58× |

## W11  E16  producers=1  consumers=8  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 7.18 [7.16–7.19] | 139 | 1.02× |
| heap_mpmc | 5 | 7.03 [7.02–7.03] | 142 | 1.00× |
| cell_spmc | 5 | 6.95 [6.87–7.06] | 144 | 0.99× |
| heap_spmc | 5 | 4.08 [3.95–4.12] | 245 | 0.58× |

## W11  E16  producers=1  consumers=8  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 7.21 ⚠ [6.32–7.22] | 139 | 1.02× |
| heap_mpmc | 5 | 7.05 ⚠ [6.26–7.06] | 142 | 1.00× |
| cell_spmc | 5 | 6.94 ⚠ [4.00–7.12] | 144 | 0.98× |
| heap_spmc | 5 | 3.96 ⚠ [2.79–4.12] | 252 | 0.56× |

## W11  E256  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 24.56 [24.55–24.63] | 40.71 | 4.33× |
| cell_mpmc | 5 | 21.83 [21.63–21.85] | 45.81 | 3.85× |
| heap_spmc | 5 | 7.90 ⚠ [7.08–8.27] | 127 | 1.39× |
| heap_mpmc | 5 | 5.67 [5.63–5.81] | 176 | 1.00× |

## W11  E256  producers=1  consumers=1  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 24.59 [24.53–24.61] | 40.67 | 1.59× |
| cell_mpmc | 5 | 22.03 [21.97–22.06] | 45.39 | 1.42× |
| heap_mpmc | 5 | 15.47 ⚠ [14.39–16.21] | 64.62 | 1.00× |
| heap_spmc | 5 | 14.16 [14.13–14.21] | 70.64 | 0.91× |

## W11  E256  producers=1  consumers=1  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 37.87 ⚠ [36.25–39.45] | 26.41 | 1.15× |
| heap_mpmc | 5 | 32.96 ⚠ [27.85–33.15] | 30.34 | 1.00× |
| cell_spmc | 5 | 24.60 [24.54–24.62] | 40.66 | 0.75× |
| cell_mpmc | 5 | 22.00 [21.94–22.05] | 45.45 | 0.67× |

## W11  E256  producers=1  consumers=1  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 7.11 [7.11–7.11] | 141 | 1.70× |
| cell_mpmc | 5 | 6.84 [6.84–6.85] | 146 | 1.64× |
| heap_spmc | 5 | 4.66 [4.61–4.71] | 214 | 1.12× |
| heap_mpmc | 5 | 4.18 [4.15–4.26] | 239 | 1.00× |

## W11  E256  producers=1  consumers=1  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 7.78 [7.77–7.79] | 129 | 1.00× |
| cell_spmc | 5 | 7.11 [7.09–7.11] | 141 | 0.91× |
| cell_mpmc | 5 | 6.88 [6.88–6.88] | 145 | 0.88× |
| heap_spmc | 5 | 6.10 [6.09–6.11] | 164 | 0.78× |

## W11  E256  producers=1  consumers=1  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 8.29 [8.17–8.34] | 121 | 1.01× |
| heap_mpmc | 5 | 8.18 [8.02–8.19] | 122 | 1.00× |
| cell_spmc | 5 | 7.11 [7.10–7.12] | 141 | 0.87× |
| cell_mpmc | 5 | 6.87 [6.87–6.88] | 145 | 0.84× |

## W11  E256  producers=1  consumers=1  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 0.96 [0.96–0.97] | 1,037 | 1.01× |
| heap_spmc | 5 | 0.96 [0.96–0.97] | 1,041 | 1.00× |
| cell_mpmc | 5 | 0.96 [0.96–0.96] | 1,042 | 1.00× |
| heap_mpmc | 5 | 0.96 [0.95–0.96] | 1,043 | 1.00× |

## W11  E256  producers=1  consumers=1  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 0.97 [0.97–0.97] | 1,027 | 1.00× |
| cell_spmc | 5 | 0.96 [0.96–0.97] | 1,036 | 0.99× |
| cell_mpmc | 5 | 0.96 [0.96–0.96] | 1,041 | 0.99× |
| heap_spmc | 5 | 0.95 [0.95–0.95] | 1,053 | 0.97× |

## W11  E256  producers=1  consumers=1  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 0.98 [0.98–0.98] | 1,018 | 1.00× |
| heap_mpmc | 5 | 0.98 [0.98–0.98] | 1,020 | 1.00× |
| cell_spmc | 5 | 0.97 [0.96–0.97] | 1,035 | 0.99× |
| cell_mpmc | 5 | 0.96 [0.96–0.96] | 1,042 | 0.98× |

## W11  E256  producers=1  consumers=2  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 19.02 ⚠ [16.69–19.99] | 52.56 | 6.02× |
| cell_mpmc | 5 | 9.23 [9.14–9.28] | 108 | 2.92× |
| heap_spmc | 5 | 6.10 [5.96–6.14] | 164 | 1.93× |
| heap_mpmc | 5 | 3.16 ⚠ [3.02–3.41] | 316 | 1.00× |

## W11  E256  producers=1  consumers=2  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 19.19 ⚠ [16.29–19.75] | 52.12 | 1.80× |
| heap_mpmc | 5 | 10.63 [10.49–10.85] | 94.03 | 1.00× |
| cell_mpmc | 5 | 9.65 [9.61–9.71] | 104 | 0.91× |
| heap_spmc | 5 | 7.86 [7.78–7.88] | 127 | 0.74× |

## W11  E256  producers=1  consumers=2  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 19.51 ⚠ [15.61–19.75] | 51.25 | 1.00× |
| cell_spmc | 5 | 18.52 ⚠ [15.18–19.46] | 53.99 | 0.95× |
| heap_spmc | 5 | 14.06 [13.96–14.24] | 71.10 | 0.72× |
| cell_mpmc | 5 | 9.77 ⚠ [9.61–15.39] | 102 | 0.50× |

## W11  E256  producers=1  consumers=2  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.63 [13.60–13.74] | 73.35 | 3.13× |
| cell_mpmc | 5 | 11.41 [11.36–11.48] | 87.61 | 2.62× |
| heap_spmc | 5 | 4.69 [4.60–4.70] | 213 | 1.08× |
| heap_mpmc | 5 | 4.35 [4.33–4.39] | 230 | 1.00× |

## W11  E256  producers=1  consumers=2  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.60 [13.55–13.66] | 73.55 | 1.31× |
| cell_mpmc | 5 | 11.45 [11.39–11.49] | 87.33 | 1.10× |
| heap_mpmc | 5 | 10.39 [10.37–10.40] | 96.28 | 1.00× |
| heap_spmc | 5 | 7.09 [7.01–7.11] | 141 | 0.68× |

## W11  E256  producers=1  consumers=2  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.62 [13.56–13.67] | 73.44 | 1.25× |
| cell_mpmc | 5 | 11.56 [11.43–11.57] | 86.49 | 1.06× |
| heap_spmc | 5 | 10.99 [10.86–11.08] | 91.00 | 1.01× |
| heap_mpmc | 5 | 10.89 [10.86–10.92] | 91.81 | 1.00× |

## W11  E256  producers=1  consumers=2  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.91 [1.91–1.92] | 522 | 1.12× |
| cell_mpmc | 5 | 1.88 [1.88–1.88] | 531 | 1.10× |
| heap_spmc | 5 | 1.72 [1.71–1.75] | 582 | 1.01× |
| heap_mpmc | 5 | 1.71 [1.70–1.72] | 585 | 1.00× |

## W11  E256  producers=1  consumers=2  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.91 [1.91–1.92] | 523 | 1.04× |
| cell_mpmc | 5 | 1.88 [1.88–1.88] | 531 | 1.02× |
| heap_mpmc | 5 | 1.85 [1.85–1.85] | 541 | 1.00× |
| heap_spmc | 5 | 1.75 [1.75–1.79] | 571 | 0.95× |

## W11  E256  producers=1  consumers=2  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.91 [1.91–1.92] | 523 | 1.03× |
| cell_mpmc | 5 | 1.89 [1.88–1.89] | 530 | 1.02× |
| heap_spmc | 5 | 1.86 [1.85–1.87] | 538 | 1.00× |
| heap_mpmc | 5 | 1.86 [1.86–1.86] | 538 | 1.00× |

## W11  E256  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.18 [12.94–13.33] | 75.87 | 5.82× |
| cell_mpmc | 5 | 8.57 [8.47–8.63] | 117 | 3.79× |
| heap_spmc | 5 | 3.34 ⚠ [2.62–3.57] | 300 | 1.47× |
| heap_mpmc | 5 | 2.26 ⚠ [2.11–2.36] | 442 | 1.00× |

## W11  E256  producers=1  consumers=4  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.31 [13.09–13.59] | 75.15 | 1.46× |
| heap_mpmc | 5 | 9.12 [9.05–9.19] | 110 | 1.00× |
| cell_mpmc | 5 | 8.79 [8.56–8.86] | 114 | 0.96× |
| heap_spmc | 5 | 3.09 [3.07–3.10] | 324 | 0.34× |

## W11  E256  producers=1  consumers=4  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.24 [13.00–13.40] | 75.52 | 1.31× |
| heap_spmc | 5 | 10.64 [10.39–10.68] | 94.01 | 1.06× |
| heap_mpmc | 5 | 10.08 [10.03–10.22] | 99.24 | 1.00× |
| cell_mpmc | 5 | 8.89 [8.69–8.92] | 112 | 0.88× |

## W11  E256  producers=1  consumers=4  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 11.45 ⚠ [8.84–18.15] | 87.32 | 4.91× |
| cell_spmc | 5 | 9.33 [9.15–9.38] | 107 | 4.00× |
| heap_spmc | 5 | 3.43 ⚠ [2.78–3.80] | 292 | 1.47× |
| heap_mpmc | 5 | 2.33 [2.26–2.35] | 429 | 1.00× |

## W11  E256  producers=1  consumers=4  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 9.75 [9.66–9.90] | 103 | 1.00× |
| cell_spmc | 5 | 9.28 [9.03–9.36] | 108 | 0.95× |
| cell_mpmc | 5 | 9.22 [8.94–9.26] | 108 | 0.95× |
| heap_spmc | 5 | 3.48 [3.44–3.49] | 287 | 0.36× |

## W11  E256  producers=1  consumers=4  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 15.31 ⚠ [10.92–19.78] | 65.30 | 1.00× |
| cell_spmc | 5 | 9.22 [9.08–9.47] | 108 | 0.60× |
| cell_mpmc | 5 | 9.20 ⚠ [9.14–14.24] | 109 | 0.60× |
| heap_spmc | 5 | 8.64 [8.51–8.72] | 116 | 0.56× |

## W11  E256  producers=1  consumers=4  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.81 [3.81–3.82] | 262 | 1.28× |
| cell_mpmc | 5 | 3.75 [3.75–3.76] | 267 | 1.26× |
| heap_spmc | 5 | 3.05 [3.04–3.05] | 328 | 1.03× |
| heap_mpmc | 5 | 2.97 [2.95–2.97] | 337 | 1.00× |

## W11  E256  producers=1  consumers=4  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.81 [3.81–3.82] | 262 | 1.05× |
| cell_mpmc | 5 | 3.76 [3.75–3.77] | 266 | 1.03× |
| heap_mpmc | 5 | 3.64 [3.64–3.64] | 275 | 1.00× |
| heap_spmc | 5 | 3.44 [3.44–3.45] | 291 | 0.95× |

## W11  E256  producers=1  consumers=4  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.82 [3.81–3.82] | 262 | 1.04× |
| cell_mpmc | 5 | 3.76 [3.75–3.76] | 266 | 1.02× |
| heap_spmc | 5 | 3.69 [3.67–3.69] | 271 | 1.00× |
| heap_mpmc | 5 | 3.67 [3.65–3.67] | 272 | 1.00× |

## W11  E256  producers=1  consumers=8  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.75 ⚠ [5.23–5.91] | 174 | 5.35× |
| heap_spmc | 5 | 2.40 ⚠ [1.88–2.52] | 416 | 2.23× |
| cell_mpmc | 5 | 2.35 ⚠ [1.82–2.47] | 425 | 2.19× |
| heap_mpmc | 5 | 1.08 ⚠ [0.94–1.20] | 930 | 1.00× |

## W11  E256  producers=1  consumers=8  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.76 ⚠ [5.19–5.92] | 174 | 2.66× |
| cell_mpmc | 5 | 2.41 ⚠ [1.81–2.46] | 415 | 1.12× |
| heap_mpmc | 5 | 2.16 ⚠ [1.57–2.25] | 462 | 1.00× |
| heap_spmc | 5 | 0.93 ⚠ [0.90–0.99] | 1,074 | 0.43× |

## W11  E256  producers=1  consumers=8  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.66 [5.50–5.78] | 177 | 2.28× |
| heap_spmc | 5 | 4.84 [4.75–4.96] | 207 | 1.95× |
| heap_mpmc | 5 | 2.49 [2.44–2.55] | 402 | 1.00× |
| cell_mpmc | 5 | 2.41 [2.36–2.47] | 414 | 0.97× |

## W11  E256  producers=1  consumers=8  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.57 [4.48–4.79] | 219 | 3.81× |
| heap_spmc | 5 | 2.47 ⚠ [2.29–2.66] | 404 | 2.07× |
| cell_mpmc | 5 | 2.46 [2.40–2.50] | 407 | 2.05× |
| heap_mpmc | 5 | 1.20 ⚠ [0.91–1.26] | 835 | 1.00× |

## W11  E256  producers=1  consumers=8  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.66 ⚠ [4.31–4.71] | 214 | 2.05× |
| cell_mpmc | 5 | 2.47 ⚠ [1.69–2.48] | 406 | 1.08× |
| heap_mpmc | 5 | 2.28 ⚠ [1.56–2.31] | 439 | 1.00× |
| heap_spmc | 5 | 0.96 [0.94–0.99] | 1,038 | 0.42× |

## W11  E256  producers=1  consumers=8  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.65 ⚠ [4.47–4.85] | 215 | 1.78× |
| heap_spmc | 5 | 4.10 [3.91–4.16] | 244 | 1.57× |
| heap_mpmc | 5 | 2.61 [2.58–2.69] | 383 | 1.00× |
| cell_mpmc | 5 | 2.47 [2.43–2.54] | 404 | 0.95× |

## W11  E256  producers=1  consumers=8  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 6.91 [6.90–6.91] | 145 | 4.45× |
| cell_spmc | 5 | 5.31 [5.11–5.54] | 188 | 3.42× |
| heap_spmc | 5 | 2.14 ⚠ [2.10–2.31] | 466 | 1.38× |
| heap_mpmc | 5 | 1.55 ⚠ [1.37–1.60] | 644 | 1.00× |

## W11  E256  producers=1  consumers=8  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 6.91 [6.46–6.91] | 145 | 1.05× |
| heap_mpmc | 5 | 6.55 [6.29–6.57] | 153 | 1.00× |
| cell_spmc | 5 | 5.08 ⚠ [4.94–5.63] | 197 | 0.77× |
| heap_spmc | 5 | 1.15 [1.12–1.16] | 870 | 0.18× |

## W11  E256  producers=1  consumers=8  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 6.92 [6.91–6.94] | 145 | 1.03× |
| heap_mpmc | 5 | 6.70 ⚠ [6.19–6.71] | 149 | 1.00× |
| cell_spmc | 5 | 5.19 ⚠ [5.03–5.44] | 193 | 0.77× |
| heap_spmc | 5 | 4.14 [4.09–4.23] | 242 | 0.62× |

## W11  E64  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 60.03 [59.84–60.12] | 16.66 | 7.55× |
| cell_mpmc | 5 | 13.22 [13.12–13.64] | 75.62 | 1.66× |
| heap_spmc | 5 | 12.98 [12.92–13.21] | 77.03 | 1.63× |
| heap_mpmc | 5 | 7.95 [7.92–8.00] | 126 | 1.00× |

## W11  E64  producers=1  consumers=1  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 59.78 [59.56–60.07] | 16.73 | 3.99× |
| heap_spmc | 5 | 27.29 [26.87–27.35] | 36.64 | 1.82× |
| heap_mpmc | 5 | 14.99 [14.66–15.02] | 66.72 | 1.00× |
| cell_mpmc | 5 | 12.88 [12.73–13.10] | 77.67 | 0.86× |

## W11  E64  producers=1  consumers=1  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 83.08 [81.00–84.05] | 12.04 | 5.25× |
| cell_spmc | 5 | 60.08 [60.05–60.09] | 16.64 | 3.80× |
| heap_mpmc | 5 | 15.81 [15.68–16.26] | 63.25 | 1.00× |
| cell_mpmc | 5 | 13.97 [13.63–14.36] | 71.57 | 0.88× |

## W11  E64  producers=1  consumers=1  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 8.57 [8.57–8.58] | 117 | 1.04× |
| cell_mpmc | 5 | 8.24 [8.24–8.24] | 121 | 1.00× |
| heap_mpmc | 5 | 8.22 [8.22–8.24] | 122 | 1.00× |
| heap_spmc | 5 | 7.95 [7.94–7.97] | 126 | 0.97× |

## W11  E64  producers=1  consumers=1  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 10.12 [10.10–10.14] | 98.81 | 1.00× |
| cell_spmc | 5 | 8.58 [8.57–8.59] | 117 | 0.85× |
| cell_mpmc | 5 | 8.25 [8.25–8.26] | 121 | 0.82× |
| heap_spmc | 5 | 7.56 [7.55–7.57] | 132 | 0.75× |

## W11  E64  producers=1  consumers=1  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 10.64 [10.60–10.67] | 93.97 | 1.02× |
| heap_mpmc | 5 | 10.44 [10.39–10.46] | 95.76 | 1.00× |
| cell_spmc | 5 | 8.58 [8.57–8.58] | 117 | 0.82× |
| cell_mpmc | 5 | 8.25 [8.24–8.25] | 121 | 0.79× |

## W11  E64  producers=1  consumers=1  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 1.00 [1.00–1.00] | 1,001 | 1.00× |
| heap_spmc | 5 | 0.99 [0.99–0.99] | 1,008 | 0.99× |
| cell_spmc | 5 | 0.99 [0.99–0.99] | 1,012 | 0.99× |
| cell_mpmc | 5 | 0.98 [0.98–0.98] | 1,017 | 0.98× |

## W11  E64  producers=1  consumers=1  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 1.00 [1.00–1.00] | 1,001 | 1.00× |
| cell_spmc | 5 | 0.99 [0.99–0.99] | 1,012 | 0.99× |
| cell_mpmc | 5 | 0.98 [0.98–0.98] | 1,017 | 0.98× |
| heap_spmc | 5 | 0.97 [0.97–0.98] | 1,026 | 0.98× |

## W11  E64  producers=1  consumers=1  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 1.01 [1.01–1.01] | 991 | 1.00× |
| heap_mpmc | 5 | 1.01 [1.00–1.01] | 995 | 1.00× |
| cell_spmc | 5 | 0.99 [0.99–0.99] | 1,012 | 0.98× |
| cell_mpmc | 5 | 0.98 [0.98–0.98] | 1,017 | 0.98× |

## W11  E64  producers=1  consumers=2  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 23.46 [22.46–23.80] | 42.62 | 3.49× |
| cell_mpmc | 5 | 10.57 [10.53–10.61] | 94.61 | 1.57× |
| heap_spmc | 5 | 8.90 [8.86–8.99] | 112 | 1.32× |
| heap_mpmc | 5 | 6.72 [6.60–6.79] | 149 | 1.00× |

## W11  E64  producers=1  consumers=2  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 22.59 [21.79–22.84] | 44.27 | 2.00× |
| heap_mpmc | 5 | 11.32 [11.17–11.43] | 88.37 | 1.00× |
| cell_mpmc | 5 | 10.74 [10.69–10.80] | 93.11 | 0.95× |
| heap_spmc | 5 | 9.26 [9.15–9.35] | 108 | 0.82× |

## W11  E64  producers=1  consumers=2  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 22.56 ⚠ [21.53–23.51] | 44.32 | 1.85× |
| heap_spmc | 5 | 14.69 ⚠ [13.33–14.82] | 68.07 | 1.21× |
| heap_mpmc | 5 | 12.18 ⚠ [12.06–21.85] | 82.11 | 1.00× |
| cell_mpmc | 5 | 10.98 [10.91–11.03] | 91.11 | 0.90× |

## W11  E64  producers=1  consumers=2  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 14.72 [14.55–14.83] | 67.92 | 1.51× |
| cell_mpmc | 5 | 13.02 [12.99–13.07] | 76.80 | 1.34× |
| heap_spmc | 5 | 10.68 [10.58–10.91] | 93.65 | 1.10× |
| heap_mpmc | 5 | 9.72 ⚠ [8.76–10.78] | 103 | 1.00× |

## W11  E64  producers=1  consumers=2  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 14.71 [14.54–14.81] | 67.96 | 1.17× |
| cell_mpmc | 5 | 13.02 [12.99–13.09] | 76.82 | 1.04× |
| heap_mpmc | 5 | 12.57 [12.56–12.62] | 79.54 | 1.00× |
| heap_spmc | 5 | 8.72 [8.70–8.80] | 115 | 0.69× |

## W11  E64  producers=1  consumers=2  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 14.72 [14.58–14.80] | 67.94 | 1.21× |
| cell_mpmc | 5 | 13.15 [13.10–13.39] | 76.03 | 1.08× |
| heap_spmc | 5 | 12.26 [12.03–12.38] | 81.53 | 1.01× |
| heap_mpmc | 5 | 12.15 [12.04–12.24] | 82.31 | 1.00× |

## W11  E64  producers=1  consumers=2  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.96 [1.96–1.96] | 511 | 1.04× |
| cell_mpmc | 5 | 1.94 [1.93–1.94] | 517 | 1.03× |
| heap_mpmc | 5 | 1.88 [1.88–1.88] | 533 | 1.00× |
| heap_spmc | 5 | 1.87 [1.86–1.87] | 536 | 0.99× |

## W11  E64  producers=1  consumers=2  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.96 [1.96–1.96] | 510 | 1.03× |
| cell_mpmc | 5 | 1.93 [1.93–1.93] | 518 | 1.02× |
| heap_mpmc | 5 | 1.90 [1.90–1.90] | 526 | 1.00× |
| heap_spmc | 5 | 1.80 [1.79–1.80] | 557 | 0.94× |

## W11  E64  producers=1  consumers=2  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.96 [1.96–1.96] | 510 | 1.04× |
| cell_mpmc | 5 | 1.93 [1.93–1.93] | 517 | 1.02× |
| heap_mpmc | 5 | 1.89 [1.89–1.89] | 528 | 1.00× |
| heap_spmc | 5 | 1.89 [1.88–1.89] | 530 | 1.00× |

## W11  E64  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 14.67 ⚠ [13.38–15.36] | 68.15 | 2.37× |
| cell_mpmc | 5 | 9.40 [9.37–9.42] | 106 | 1.52× |
| heap_spmc | 5 | 6.79 [6.69–6.83] | 147 | 1.10× |
| heap_mpmc | 5 | 6.18 [6.16–6.35] | 162 | 1.00× |

## W11  E64  producers=1  consumers=4  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 14.29 ⚠ [13.63–16.22] | 70.00 | 1.48× |
| heap_mpmc | 5 | 9.68 [9.60–9.70] | 103 | 1.00× |
| cell_mpmc | 5 | 9.50 [9.45–9.52] | 105 | 0.98× |
| heap_spmc | 5 | 3.48 [3.47–3.50] | 287 | 0.36× |

## W11  E64  producers=1  consumers=4  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 14.53 ⚠ [12.72–15.26] | 68.84 | 1.47× |
| heap_spmc | 5 | 11.57 ⚠ [10.27–11.76] | 86.44 | 1.17× |
| heap_mpmc | 5 | 9.89 [9.87–9.96] | 101 | 1.00× |
| cell_mpmc | 5 | 9.62 [9.55–9.65] | 104 | 0.97× |

## W11  E64  producers=1  consumers=4  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 9.75 ⚠ [9.71–20.45] | 103 | 1.42× |
| cell_spmc | 5 | 9.07 [8.96–9.37] | 110 | 1.32× |
| heap_spmc | 5 | 8.08 [7.99–8.27] | 124 | 1.18× |
| heap_mpmc | 5 | 6.86 [6.79–6.90] | 146 | 1.00× |

## W11  E64  producers=1  consumers=4  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 10.54 ⚠ [10.50–15.41] | 94.91 | 1.00× |
| cell_mpmc | 5 | 9.77 [9.69–9.89] | 102 | 0.93× |
| cell_spmc | 5 | 9.29 [9.06–9.46] | 108 | 0.88× |
| heap_spmc | 5 | 4.24 [4.22–4.26] | 236 | 0.40× |

## W11  E64  producers=1  consumers=4  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 11.18 ⚠ [11.07–15.16] | 89.48 | 1.00× |
| cell_mpmc | 5 | 9.88 ⚠ [9.83–22.74] | 101 | 0.88× |
| cell_spmc | 5 | 9.14 [8.95–9.59] | 109 | 0.82× |
| heap_spmc | 5 | 8.45 [8.22–8.57] | 118 | 0.76× |

## W11  E64  producers=1  consumers=4  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.90 [3.90–3.91] | 256 | 1.06× |
| cell_mpmc | 5 | 3.85 [3.85–3.85] | 260 | 1.04× |
| heap_spmc | 5 | 3.72 [3.72–3.73] | 269 | 1.01× |
| heap_mpmc | 5 | 3.70 [3.69–3.70] | 270 | 1.00× |

## W11  E64  producers=1  consumers=4  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.91 [3.90–3.91] | 256 | 1.04× |
| cell_mpmc | 5 | 3.85 [3.84–3.85] | 260 | 1.02× |
| heap_mpmc | 5 | 3.76 [3.75–3.76] | 266 | 1.00× |
| heap_spmc | 5 | 3.58 [3.57–3.58] | 280 | 0.95× |

## W11  E64  producers=1  consumers=4  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.90 [3.90–3.91] | 256 | 1.04× |
| cell_mpmc | 5 | 3.85 [3.81–3.85] | 260 | 1.03× |
| heap_spmc | 5 | 3.76 [3.75–3.77] | 266 | 1.00× |
| heap_mpmc | 5 | 3.75 [3.75–3.76] | 267 | 1.00× |

## W11  E64  producers=1  consumers=8  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 6.37 ⚠ [6.12–7.63] | 157 | 3.33× |
| heap_spmc | 5 | 3.21 ⚠ [2.47–3.24] | 311 | 1.68× |
| cell_mpmc | 5 | 2.29 ⚠ [1.61–2.42] | 436 | 1.20× |
| heap_mpmc | 5 | 1.91 ⚠ [1.35–2.00] | 522 | 1.00× |

## W11  E64  producers=1  consumers=8  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 6.37 ⚠ [6.24–7.16] | 157 | 2.65× |
| heap_mpmc | 5 | 2.40 ⚠ [1.55–2.43] | 416 | 1.00× |
| cell_mpmc | 5 | 2.30 ⚠ [1.72–2.37] | 434 | 0.96× |
| heap_spmc | 5 | 1.14 ⚠ [1.06–1.21] | 876 | 0.48× |

## W11  E64  producers=1  consumers=8  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 6.41 ⚠ [6.00–7.31] | 156 | 2.49× |
| heap_spmc | 5 | 4.85 ⚠ [4.04–5.04] | 206 | 1.89× |
| heap_mpmc | 5 | 2.57 ⚠ [1.71–2.66] | 389 | 1.00× |
| cell_mpmc | 5 | 2.35 ⚠ [1.64–2.42] | 426 | 0.91× |

## W11  E64  producers=1  consumers=8  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.15 ⚠ [3.97–5.54] | 194 | 2.42× |
| heap_spmc | 5 | 3.48 ⚠ [2.71–3.51] | 287 | 1.63× |
| cell_mpmc | 5 | 2.41 ⚠ [1.73–2.54] | 414 | 1.13× |
| heap_mpmc | 5 | 2.13 ⚠ [1.46–2.20] | 470 | 1.00× |

## W11  E64  producers=1  consumers=8  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.08 ⚠ [4.90–5.38] | 197 | 2.02× |
| heap_mpmc | 5 | 2.51 ⚠ [1.69–2.58] | 398 | 1.00× |
| cell_mpmc | 5 | 2.37 ⚠ [2.33–2.56] | 422 | 0.95× |
| heap_spmc | 5 | 1.03 ⚠ [1.02–1.12] | 973 | 0.41× |

## W11  E64  producers=1  consumers=8  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 5.08 ⚠ [3.98–5.28] | 197 | 1.91× |
| heap_spmc | 5 | 4.30 ⚠ [2.93–4.44] | 233 | 1.61× |
| heap_mpmc | 5 | 2.67 ⚠ [1.86–2.84] | 375 | 1.00× |
| cell_mpmc | 5 | 2.57 ⚠ [1.93–2.58] | 390 | 0.96× |

## W11  E64  producers=1  consumers=8  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 7.11 ⚠ [6.29–7.12] | 141 | 1.09× |
| heap_mpmc | 5 | 6.55 ⚠ [2.50–6.57] | 153 | 1.00× |
| cell_spmc | 5 | 5.64 ⚠ [3.46–6.44] | 177 | 0.86× |
| heap_spmc | 5 | 3.99 ⚠ [2.85–4.03] | 251 | 0.61× |

## W11  E64  producers=1  consumers=8  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 7.10 ⚠ [6.30–7.11] | 141 | 1.04× |
| heap_mpmc | 5 | 6.81 [6.65–6.83] | 147 | 1.00× |
| cell_spmc | 5 | 5.89 ⚠ [3.56–6.54] | 170 | 0.86× |
| heap_spmc | 5 | 1.39 [1.34–1.41] | 717 | 0.20× |

## W11  E64  producers=1  consumers=8  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 7.13 ⚠ [6.30–7.14] | 140 | 1.04× |
| heap_mpmc | 5 | 6.87 [6.40–6.88] | 146 | 1.00× |
| cell_spmc | 5 | 6.26 ⚠ [3.51–6.41] | 160 | 0.91× |
| heap_spmc | 5 | 4.17 ⚠ [2.97–4.28] | 240 | 0.61× |

## W11  [mechanism pass]  E16  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 543 | 167 | 7.01 | 0.31 | 3.50 |
| cell_spmc | 1 | 0.00 | 0.00 | 61.04 | 118 | 0.42 | 1.94 | 3.50 |
| heap_mpmc | 1 | 0.00 | 0.00 | 558 | 236 | 6.74 | 0.42 | 3.50 |
| heap_spmc | 1 | 0.00 | 0.00 | 68.54 | 175 | 0.05 | 2.55 | 4.00 |

## W11  [mechanism pass]  E16  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 2,052 | 278 | 15.74 | 0.14 | 5.91 |
| cell_spmc | 1 | 0.00 | 0.00 | 921 | 250 | 3.57 | 0.27 | 3.50 |
| heap_mpmc | 1 | 0.00 | 0.00 | 2,030 | 348 | 15.47 | 0.17 | 6.04 |
| heap_spmc | 1 | 0.00 | 0.00 | 1,011 | 321 | 4.58 | 0.32 | 4.00 |

## W11  [mechanism pass]  E256  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 367 | 372 | 0.27 | 1.01 | 9.40 |
| cell_spmc | 1 | 0.00 | 0.00 | 335 | 327 | 4.37 | 0.98 | 5.50 |
| heap_mpmc | 1 | 1.00 | 1.00 | 967 | 934 | 14.55 | 0.97 | 6.50 |
| heap_spmc | 1 | 1.00 | 1.00 | 724 | 865 | 10.74 | 1.20 | 6.00 |

## W11  [mechanism pass]  E256  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 2,380 | 515 | 26.60 | 0.22 | 14.63 |
| cell_spmc | 1 | 0.00 | 0.00 | 1,560 | 500 | 12.52 | 0.32 | 6.00 |
| heap_mpmc | 1 | 1.00 | 1.00 | 6,809 | 2,171 | 45.51 | 0.32 | 6.00 |
| heap_spmc | 1 | 1.00 | 1.00 | 3,323 | 955 | 45.63 | 0.29 | 4.00 |

## W11  [mechanism pass]  E64  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 609 | 206 | 8.60 | 0.34 | 4.80 |
| cell_spmc | 1 | 0.00 | 0.00 | 138 | 156 | 1.17 | 1.14 | 4.00 |
| heap_mpmc | 1 | 1.00 | 1.00 | 1,423 | 602 | 21.42 | 0.42 | 3.50 |
| heap_spmc | 1 | 1.00 | 1.00 | 963 | 515 | 15.54 | 0.54 | 3.50 |

## W11  [mechanism pass]  E64  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 2,182 | 343 | 19.18 | 0.16 | 7.79 |
| cell_spmc | 1 | 0.00 | 0.00 | 1,282 | 319 | 5.83 | 0.25 | 4.00 |
| heap_mpmc | 1 | 1.00 | 1.00 | 4,226 | 963 | 34.21 | 0.23 | 6.00 |
| heap_spmc | 1 | 1.00 | 1.00 | 3,416 | 635 | 38.43 | 0.19 | 3.50 |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
