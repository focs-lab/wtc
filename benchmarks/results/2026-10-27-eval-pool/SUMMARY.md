# eval-pool  (2026-10-26T17:35:35Z)

AMD EPYC 9115 16-Core Processor, kernel 6.8.0-137-generic, allowed cpus 16-31,48-63, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=0;`, git 

Control (shim_generic E64, median of 3 and 3): open 13.94, close 16.00 Mops/s, drift 12.9% — **NOISY: rerun this leg**

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.055, rep 2 1.132, rep 3 1.163, rep 4 1.182 (n=432) — **drifting in one direction: treat ratios measured far apart with care**.  Widest single row: cell_mpmc in W11  E256  producers=1  consumers=2  work=0  alloc=snmalloc, 243% across repetitions.

## W11  E16  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 153 ⚠ [131–154] | 6.52 | 10.29× |
| heap_spmc | 5 | 121 ⚠ [95.61–121] | 8.26 | 8.12× |
| heap_mpmc | 5 | 14.91 ⚠ [12.10–14.96] | 67.07 | 1.00× |
| cell_mpmc | 5 | 14.76 ⚠ [12.68–15.27] | 67.77 | 0.99× |

## W11  E16  producers=1  consumers=1  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 129 ⚠ [109–152] | 7.75 | 9.58× |
| heap_spmc | 5 | 104 ⚠ [92.67–121] | 9.61 | 7.72× |
| heap_mpmc | 5 | 13.48 ⚠ [12.47–14.22] | 74.19 | 1.00× |
| cell_mpmc | 5 | 13.04 ⚠ [12.79–14.41] | 76.71 | 0.97× |

## W11  E16  producers=1  consumers=1  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 149 ⚠ [106–149] | 6.71 | 9.97× |
| heap_spmc | 5 | 118 ⚠ [82.76–120] | 8.45 | 7.92× |
| heap_mpmc | 5 | 14.96 ⚠ [12.33–15.21] | 66.85 | 1.00× |
| cell_mpmc | 5 | 14.54 ⚠ [12.80–14.97] | 68.79 | 0.97× |

## W11  E16  producers=1  consumers=1  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 14.94 ⚠ [12.94–15.03] | 66.92 | 1.06× |
| cell_spmc | 5 | 14.94 ⚠ [13.14–15.37] | 66.94 | 1.06× |
| heap_mpmc | 5 | 14.14 ⚠ [11.85–14.16] | 70.74 | 1.00× |
| heap_spmc | 5 | 14.07 ⚠ [12.32–14.19] | 71.07 | 1.00× |

## W11  E16  producers=1  consumers=1  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.41 ⚠ [12.69–15.34] | 74.55 | 1.16× |
| cell_mpmc | 5 | 13.19 ⚠ [12.14–15.03] | 75.80 | 1.14× |
| heap_spmc | 5 | 12.40 ⚠ [9.56–14.22] | 80.61 | 1.07× |
| heap_mpmc | 5 | 11.56 ⚠ [8.68–14.17] | 86.52 | 1.00× |

## W11  E16  producers=1  consumers=1  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 15.02 ⚠ [11.10–15.03] | 66.59 | 1.06× |
| heap_mpmc | 5 | 14.13 ⚠ [8.64–14.14] | 70.78 | 1.00× |
| heap_spmc | 5 | 14.10 ⚠ [8.68–14.21] | 70.91 | 1.00× |
| cell_spmc | 5 | 13.46 ⚠ [10.43–15.33] | 74.27 | 0.95× |

## W11  E16  producers=1  consumers=1  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.16 ⚠ [0.99–1.16] | 862 | 1.00× |
| cell_mpmc | 5 | 1.16 ⚠ [1.01–1.16] | 864 | 1.00× |
| heap_mpmc | 5 | 1.16 ⚠ [0.99–1.16] | 866 | 1.00× |
| heap_spmc | 5 | 1.16 ⚠ [1.00–1.16] | 866 | 1.00× |

## W11  E16  producers=1  consumers=1  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.02 ⚠ [0.95–1.16] | 982 | 1.00× |
| heap_mpmc | 5 | 1.02 ⚠ [0.98–1.16] | 985 | 1.00× |
| cell_mpmc | 5 | 1.01 ⚠ [0.95–1.15] | 988 | 1.00× |
| heap_spmc | 5 | 0.99 ⚠ [0.94–1.15] | 1,015 | 0.97× |

## W11  E16  producers=1  consumers=1  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.02 ⚠ [1.00–1.16] | 982 | 1.00× |
| heap_mpmc | 5 | 1.01 ⚠ [0.96–1.16] | 986 | 1.00× |
| cell_mpmc | 5 | 1.01 ⚠ [1.00–1.16] | 987 | 1.00× |
| heap_spmc | 5 | 1.01 ⚠ [1.00–1.16] | 987 | 1.00× |

## W11  E16  producers=1  consumers=2  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 43.33 ⚠ [35.74–43.57] | 23.08 | 3.51× |
| heap_spmc | 5 | 30.53 ⚠ [27.25–31.04] | 32.76 | 2.47× |
| heap_mpmc | 5 | 12.35 ⚠ [10.17–24.17] | 80.99 | 1.00× |
| cell_mpmc | 5 | 11.93 ⚠ [10.06–12.16] | 83.79 | 0.97× |

## W11  E16  producers=1  consumers=2  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 38.10 ⚠ [35.15–44.12] | 26.25 | 3.50× |
| heap_spmc | 5 | 27.40 ⚠ [27.19–31.38] | 36.49 | 2.52× |
| heap_mpmc | 5 | 10.88 ⚠ [10.41–13.83] | 91.88 | 1.00× |
| cell_mpmc | 5 | 10.38 ⚠ [10.14–21.57] | 96.37 | 0.95× |

## W11  E16  producers=1  consumers=2  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 37.26 ⚠ [36.78–44.56] | 26.84 | 3.45× |
| heap_spmc | 5 | 28.00 ⚠ [26.58–31.67] | 35.71 | 2.59× |
| heap_mpmc | 5 | 10.79 ⚠ [10.67–12.56] | 92.65 | 1.00× |
| cell_mpmc | 5 | 10.73 ⚠ [9.92–12.42] | 93.19 | 0.99× |

## W11  E16  producers=1  consumers=2  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 18.76 ⚠ [14.85–18.78] | 53.31 | 1.21× |
| cell_mpmc | 5 | 17.85 ⚠ [13.93–18.05] | 56.03 | 1.15× |
| heap_spmc | 5 | 16.44 ⚠ [13.64–17.05] | 60.81 | 1.06× |
| heap_mpmc | 5 | 15.52 ⚠ [12.79–15.56] | 64.44 | 1.00× |

## W11  E16  producers=1  consumers=2  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 16.18 ⚠ [15.07–18.71] | 61.80 | 1.21× |
| cell_mpmc | 5 | 15.42 ⚠ [13.98–18.00] | 64.84 | 1.16× |
| heap_spmc | 5 | 14.27 ⚠ [13.93–16.54] | 70.08 | 1.07× |
| heap_mpmc | 5 | 13.34 ⚠ [12.60–15.62] | 74.97 | 1.00× |

## W11  E16  producers=1  consumers=2  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 16.18 ⚠ [14.82–18.82] | 61.80 | 1.22× |
| cell_mpmc | 5 | 15.48 ⚠ [14.22–18.03] | 64.58 | 1.16× |
| heap_spmc | 5 | 14.66 ⚠ [13.53–17.21] | 68.22 | 1.10× |
| heap_mpmc | 5 | 13.31 ⚠ [12.18–15.58] | 75.12 | 1.00× |

## W11  E16  producers=1  consumers=2  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 2.25 ⚠ [1.92–2.25] | 445 | 1.03× |
| cell_mpmc | 5 | 2.23 ⚠ [1.88–2.24] | 448 | 1.02× |
| heap_spmc | 5 | 2.18 ⚠ [1.86–2.19] | 458 | 1.00× |
| heap_mpmc | 5 | 2.18 ⚠ [1.86–2.18] | 459 | 1.00× |

## W11  E16  producers=1  consumers=2  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 1.94 ⚠ [1.87–2.23] | 516 | 1.03× |
| cell_spmc | 5 | 1.93 ⚠ [1.86–2.24] | 517 | 1.02× |
| heap_spmc | 5 | 1.91 ⚠ [1.87–2.18] | 524 | 1.01× |
| heap_mpmc | 5 | 1.89 ⚠ [1.87–2.16] | 529 | 1.00× |

## W11  E16  producers=1  consumers=2  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.95 ⚠ [1.91–2.25] | 512 | 1.03× |
| cell_mpmc | 5 | 1.95 ⚠ [1.86–2.24] | 514 | 1.03× |
| heap_spmc | 5 | 1.90 ⚠ [1.85–2.19] | 525 | 1.00× |
| heap_mpmc | 5 | 1.90 ⚠ [1.83–2.19] | 527 | 1.00× |

## W11  E16  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 33.90 ⚠ [28.82–34.80] | 29.50 | 3.35× |
| heap_spmc | 5 | 24.88 ⚠ [21.40–25.12] | 40.19 | 2.46× |
| heap_mpmc | 5 | 10.13 ⚠ [8.45–12.55] | 98.69 | 1.00× |
| cell_mpmc | 5 | 9.87 ⚠ [8.39–10.05] | 101 | 0.97× |

## W11  E16  producers=1  consumers=4  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 29.22 ⚠ [28.29–35.17] | 34.23 | 3.29× |
| heap_spmc | 5 | 22.11 ⚠ [21.24–25.04] | 45.22 | 2.49× |
| heap_mpmc | 5 | 8.89 ⚠ [8.36–24.42] | 112 | 1.00× |
| cell_mpmc | 5 | 8.53 ⚠ [8.33–10.10] | 117 | 0.96× |

## W11  E16  producers=1  consumers=4  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 29.94 ⚠ [29.49–35.88] | 33.40 | 3.31× |
| heap_spmc | 5 | 23.62 ⚠ [21.83–25.41] | 42.33 | 2.61× |
| heap_mpmc | 5 | 9.05 ⚠ [8.67–10.28] | 110 | 1.00× |
| cell_mpmc | 5 | 8.89 ⚠ [8.47–10.10] | 112 | 0.98× |

## W11  E16  producers=1  consumers=4  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 27.02 ⚠ [22.13–27.06] | 37.01 | 2.48× |
| heap_spmc | 5 | 27.00 ⚠ [23.08–27.16] | 37.03 | 2.47× |
| heap_mpmc | 5 | 10.91 ⚠ [10.10–21.66] | 91.62 | 1.00× |
| cell_mpmc | 5 | 10.59 ⚠ [9.82–22.40] | 94.41 | 0.97× |

## W11  E16  producers=1  consumers=4  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 23.93 ⚠ [23.38–27.03] | 41.78 | 1.74× |
| cell_spmc | 5 | 22.72 ⚠ [22.05–27.01] | 44.02 | 1.65× |
| heap_mpmc | 5 | 13.78 ⚠ [9.29–22.44] | 72.55 | 1.00× |
| cell_mpmc | 5 | 9.34 ⚠ [8.77–23.96] | 107 | 0.68× |

## W11  E16  producers=1  consumers=4  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 24.14 ⚠ [23.25–27.04] | 41.42 | 2.20× |
| cell_spmc | 5 | 22.58 ⚠ [22.02–27.09] | 44.29 | 2.06× |
| heap_mpmc | 5 | 10.97 ⚠ [9.28–21.34] | 91.16 | 1.00× |
| cell_mpmc | 5 | 10.67 ⚠ [9.00–24.78] | 93.70 | 0.97× |

## W11  E16  producers=1  consumers=4  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.45 ⚠ [3.81–4.46] | 225 | 1.03× |
| cell_mpmc | 5 | 4.41 ⚠ [3.80–4.44] | 227 | 1.02× |
| heap_spmc | 5 | 4.38 ⚠ [3.71–4.40] | 228 | 1.01× |
| heap_mpmc | 5 | 4.32 ⚠ [3.67–4.33] | 231 | 1.00× |

## W11  E16  producers=1  consumers=4  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 3.83 ⚠ [3.70–4.44] | 261 | 1.03× |
| cell_spmc | 5 | 3.83 ⚠ [3.69–4.46] | 261 | 1.03× |
| heap_spmc | 5 | 3.81 ⚠ [3.65–4.39] | 263 | 1.02× |
| heap_mpmc | 5 | 3.72 ⚠ [3.61–4.33] | 269 | 1.00× |

## W11  E16  producers=1  consumers=4  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 3.90 ⚠ [3.74–4.47] | 257 | 1.04× |
| cell_mpmc | 5 | 3.81 ⚠ [3.69–4.45] | 262 | 1.02× |
| heap_spmc | 5 | 3.77 ⚠ [3.69–4.40] | 265 | 1.01× |
| heap_mpmc | 5 | 3.73 ⚠ [3.61–4.34] | 268 | 1.00× |

## W11  E16  producers=1  consumers=8  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 42.88 ⚠ [35.30–43.13] | 23.32 | 18.09× |
| heap_spmc | 5 | 11.79 ⚠ [9.59–12.50] | 84.83 | 4.97× |
| heap_mpmc | 5 | 2.37 ⚠ [2.27–2.64] | 422 | 1.00× |
| cell_mpmc | 5 | 2.14 ⚠ [1.70–2.47] | 467 | 0.90× |

## W11  E16  producers=1  consumers=8  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 32.89 ⚠ [31.03–38.53] | 30.41 | 15.10× |
| heap_spmc | 5 | 10.92 ⚠ [10.30–12.43] | 91.56 | 5.01× |
| cell_mpmc | 5 | 2.19 ⚠ [2.05–2.42] | 457 | 1.00× |
| heap_mpmc | 5 | 2.18 ⚠ [1.71–2.27] | 459 | 1.00× |

## W11  E16  producers=1  consumers=8  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 39.59 ⚠ [34.38–41.67] | 25.26 | 16.57× |
| heap_spmc | 5 | 11.40 ⚠ [10.35–12.72] | 87.69 | 4.77× |
| heap_mpmc | 5 | 2.39 ⚠ [1.83–2.65] | 418 | 1.00× |
| cell_mpmc | 5 | 2.07 ⚠ [1.47–2.60] | 484 | 0.86× |

## W11  E16  producers=1  consumers=8  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 31.63 ⚠ [23.15–33.17] | 31.62 | 13.39× |
| heap_spmc | 5 | 22.50 ⚠ [19.99–23.55] | 44.44 | 9.53× |
| cell_mpmc | 5 | 2.45 ⚠ [2.03–3.89] | 408 | 1.04× |
| heap_mpmc | 5 | 2.36 ⚠ [2.10–2.66] | 423 | 1.00× |

## W11  E16  producers=1  consumers=8  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 24.52 ⚠ [22.87–26.82] | 40.78 | 10.76× |
| heap_spmc | 5 | 19.68 [18.98–20.00] | 50.81 | 8.63× |
| heap_mpmc | 5 | 2.28 ⚠ [1.68–2.71] | 439 | 1.00× |
| cell_mpmc | 5 | 2.15 ⚠ [1.88–2.66] | 465 | 0.94× |

## W11  E16  producers=1  consumers=8  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 28.13 ⚠ [22.81–33.25] | 35.55 | 11.35× |
| heap_spmc | 5 | 19.90 ⚠ [19.58–23.69] | 50.25 | 8.03× |
| heap_mpmc | 5 | 2.48 ⚠ [2.17–2.64] | 404 | 1.00× |
| cell_mpmc | 5 | 2.41 ⚠ [2.08–2.65] | 415 | 0.97× |

## W11  E16  producers=1  consumers=8  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 7.66 ⚠ [6.70–8.01] | 131 | 1.03× |
| cell_spmc | 5 | 7.65 ⚠ [6.44–8.02] | 131 | 1.03× |
| heap_spmc | 5 | 7.51 ⚠ [6.06–7.79] | 133 | 1.01× |
| heap_mpmc | 5 | 7.45 ⚠ [6.37–7.77] | 134 | 1.00× |

## W11  E16  producers=1  consumers=8  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 6.63 [6.56–6.84] | 151 | 1.02× |
| cell_mpmc | 5 | 6.54 ⚠ [6.17–6.87] | 153 | 1.01× |
| heap_mpmc | 5 | 6.49 ⚠ [5.88–6.71] | 154 | 1.00× |
| heap_spmc | 5 | 6.34 ⚠ [5.73–6.76] | 158 | 0.98× |

## W11  E16  producers=1  consumers=8  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 7.57 ⚠ [5.97–8.05] | 132 | 1.06× |
| cell_spmc | 5 | 7.50 ⚠ [6.39–8.08] | 133 | 1.05× |
| heap_spmc | 5 | 7.36 ⚠ [6.41–7.83] | 136 | 1.03× |
| heap_mpmc | 5 | 7.13 ⚠ [6.12–7.85] | 140 | 1.00× |

## W11  E256  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 20.92 ⚠ [19.76–24.29] | 47.79 | 4.27× |
| cell_mpmc | 5 | 18.92 ⚠ [16.90–21.63] | 52.84 | 3.87× |
| heap_spmc | 5 | 6.80 ⚠ [5.31–8.31] | 147 | 1.39× |
| heap_mpmc | 5 | 4.90 ⚠ [4.37–5.77] | 204 | 1.00× |

## W11  E256  producers=1  consumers=1  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 24.41 ⚠ [20.99–24.66] | 40.96 | 1.59× |
| heap_spmc | 5 | 24.39 ⚠ [13.78–24.49] | 41.00 | 1.59× |
| cell_mpmc | 5 | 21.95 ⚠ [17.46–22.08] | 45.56 | 1.43× |
| heap_mpmc | 5 | 15.37 ⚠ [11.52–16.18] | 65.06 | 1.00× |

## W11  E256  producers=1  consumers=1  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 36.01 ⚠ [13.61–36.33] | 27.77 | 1.25× |
| heap_mpmc | 5 | 28.76 ⚠ [15.18–34.36] | 34.77 | 1.00× |
| cell_spmc | 5 | 24.36 ⚠ [18.72–24.64] | 41.05 | 0.85× |
| cell_mpmc | 5 | 21.96 ⚠ [16.29–22.04] | 45.55 | 0.76× |

## W11  E256  producers=1  consumers=1  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 6.60 ⚠ [6.47–7.70] | 152 | 1.77× |
| cell_mpmc | 5 | 6.49 ⚠ [6.16–7.44] | 154 | 1.74× |
| heap_spmc | 5 | 4.04 ⚠ [3.63–5.01] | 248 | 1.09× |
| heap_mpmc | 5 | 3.72 ⚠ [3.42–4.34] | 269 | 1.00× |

## W11  E256  producers=1  consumers=1  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 8.53 ⚠ [7.35–8.59] | 117 | 1.01× |
| heap_mpmc | 5 | 8.42 ⚠ [7.22–8.53] | 119 | 1.00× |
| cell_spmc | 5 | 7.73 ⚠ [6.66–7.74] | 129 | 0.92× |
| cell_mpmc | 5 | 7.46 ⚠ [6.36–7.48] | 134 | 0.89× |

## W11  E256  producers=1  consumers=1  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 9.05 ⚠ [6.39–9.08] | 111 | 1.00× |
| heap_mpmc | 5 | 9.01 ⚠ [6.36–9.05] | 111 | 1.00× |
| cell_spmc | 5 | 7.71 ⚠ [6.57–7.75] | 130 | 0.86× |
| cell_mpmc | 5 | 7.46 ⚠ [6.35–7.48] | 134 | 0.83× |

## W11  E256  producers=1  consumers=1  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 0.94 ⚠ [0.89–1.08] | 1,058 | 1.00× |
| cell_spmc | 5 | 0.93 ⚠ [0.92–1.08] | 1,070 | 0.99× |
| cell_mpmc | 5 | 0.93 ⚠ [0.91–1.08] | 1,076 | 0.98× |
| heap_spmc | 5 | 0.90 ⚠ [0.90–1.07] | 1,109 | 0.95× |

## W11  E256  producers=1  consumers=1  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 1.10 ⚠ [0.93–1.10] | 913 | 1.00× |
| heap_spmc | 5 | 1.09 ⚠ [0.94–1.10] | 915 | 1.00× |
| cell_spmc | 5 | 1.09 ⚠ [0.93–1.09] | 921 | 0.99× |
| cell_mpmc | 5 | 1.08 ⚠ [0.92–1.08] | 927 | 0.98× |

## W11  E256  producers=1  consumers=1  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 1.10 ⚠ [0.92–1.11] | 906 | 1.00× |
| heap_spmc | 5 | 1.10 ⚠ [0.93–1.10] | 910 | 1.00× |
| cell_spmc | 5 | 1.08 ⚠ [0.95–1.09] | 922 | 0.98× |
| cell_mpmc | 5 | 1.08 ⚠ [0.94–1.08] | 927 | 0.98× |

## W11  E256  producers=1  consumers=2  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 23.73 ⚠ [23.42–30.38] | 42.14 | 8.40× |
| cell_mpmc | 5 | 7.65 ⚠ [7.56–23.31] | 131 | 2.71× |
| heap_spmc | 5 | 5.01 ⚠ [4.37–5.63] | 199 | 1.77× |
| heap_mpmc | 5 | 2.83 ⚠ [2.45–3.81] | 354 | 1.00× |

## W11  E256  producers=1  consumers=2  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 28.49 ⚠ [22.18–30.31] | 35.10 | 2.70× |
| heap_spmc | 5 | 23.60 ⚠ [17.66–24.03] | 42.37 | 2.24× |
| heap_mpmc | 5 | 10.55 ⚠ [8.75–10.92] | 94.82 | 1.00× |
| cell_mpmc | 5 | 9.40 ⚠ [7.95–9.55] | 106 | 0.89× |

## W11  E256  producers=1  consumers=2  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 29.66 ⚠ [25.01–30.77] | 33.72 | 1.52× |
| heap_spmc | 5 | 21.16 [20.03–21.38] | 47.26 | 1.09× |
| heap_mpmc | 5 | 19.46 ⚠ [9.31–19.80] | 51.39 | 1.00× |
| cell_mpmc | 5 | 9.48 ⚠ [7.89–27.07] | 105 | 0.49× |

## W11  E256  producers=1  consumers=2  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 11.22 ⚠ [11.15–13.04] | 89.15 | 3.13× |
| cell_mpmc | 5 | 10.79 ⚠ [10.39–12.18] | 92.67 | 3.01× |
| heap_spmc | 5 | 3.84 ⚠ [3.66–4.55] | 260 | 1.07× |
| heap_mpmc | 5 | 3.59 ⚠ [3.54–4.36] | 279 | 1.00× |

## W11  E256  producers=1  consumers=2  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.18 ⚠ [11.00–13.24] | 75.86 | 1.21× |
| cell_mpmc | 5 | 12.18 ⚠ [10.44–12.26] | 82.11 | 1.12× |
| heap_spmc | 5 | 11.70 ⚠ [9.04–11.82] | 85.50 | 1.07× |
| heap_mpmc | 5 | 10.89 ⚠ [9.01–10.99] | 91.80 | 1.00× |

## W11  E256  producers=1  consumers=2  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 13.20 ⚠ [11.25–13.26] | 75.74 | 1.14× |
| heap_spmc | 5 | 12.39 ⚠ [9.41–12.43] | 80.68 | 1.07× |
| cell_mpmc | 5 | 12.24 ⚠ [11.19–12.47] | 81.73 | 1.06× |
| heap_mpmc | 5 | 11.57 ⚠ [9.34–11.64] | 86.41 | 1.00× |

## W11  E256  producers=1  consumers=2  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.83 ⚠ [1.81–2.13] | 545 | 1.15× |
| cell_mpmc | 5 | 1.82 ⚠ [1.76–2.11] | 550 | 1.14× |
| heap_mpmc | 5 | 1.59 ⚠ [1.58–1.86] | 628 | 1.00× |
| heap_spmc | 5 | 1.58 ⚠ [1.56–1.88] | 631 | 1.00× |

## W11  E256  producers=1  consumers=2  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 2.15 ⚠ [1.81–2.15] | 466 | 1.04× |
| cell_mpmc | 5 | 2.11 ⚠ [1.79–2.12] | 474 | 1.02× |
| heap_spmc | 5 | 2.08 ⚠ [1.77–2.10] | 480 | 1.01× |
| heap_mpmc | 5 | 2.07 ⚠ [1.73–2.07] | 483 | 1.00× |

## W11  E256  producers=1  consumers=2  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 2.15 ⚠ [1.78–2.15] | 466 | 1.03× |
| cell_mpmc | 5 | 2.11 ⚠ [1.80–2.12] | 473 | 1.02× |
| heap_spmc | 5 | 2.09 ⚠ [1.75–2.09] | 478 | 1.01× |
| heap_mpmc | 5 | 2.08 ⚠ [1.74–2.08] | 482 | 1.00× |

## W11  E256  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 29.96 ⚠ [28.99–35.91] | 33.38 | 16.25× |
| cell_mpmc | 5 | 7.23 ⚠ [7.16–8.33] | 138 | 3.92× |
| heap_mpmc | 5 | 1.84 ⚠ [1.67–2.28] | 542 | 1.00× |
| heap_spmc | 5 | 1.84 ⚠ [1.75–2.16] | 545 | 1.00× |

## W11  E256  producers=1  consumers=4  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 35.95 ⚠ [30.54–36.02] | 27.82 | 3.96× |
| heap_spmc | 5 | 24.21 ⚠ [18.83–24.29] | 41.30 | 2.67× |
| heap_mpmc | 5 | 9.07 ⚠ [7.54–9.13] | 110 | 1.00× |
| cell_mpmc | 5 | 8.92 ⚠ [7.46–9.03] | 112 | 0.98× |

## W11  E256  producers=1  consumers=4  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 35.36 ⚠ [31.67–36.52] | 28.28 | 3.50× |
| heap_spmc | 5 | 23.62 [23.53–24.56] | 42.33 | 2.34× |
| heap_mpmc | 5 | 10.09 ⚠ [8.21–10.14] | 99.06 | 1.00× |
| cell_mpmc | 5 | 8.94 ⚠ [7.55–8.99] | 112 | 0.89× |

## W11  E256  producers=1  consumers=4  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 18.90 ⚠ [18.24–22.97] | 52.90 | 9.93× |
| cell_mpmc | 5 | 7.54 ⚠ [7.11–18.17] | 133 | 3.96× |
| heap_spmc | 5 | 2.01 ⚠ [1.87–2.60] | 498 | 1.05× |
| heap_mpmc | 5 | 1.90 ⚠ [1.87–2.12] | 525 | 1.00× |

## W11  E256  producers=1  consumers=4  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 23.12 ⚠ [18.27–23.16] | 43.26 | 2.42× |
| heap_spmc | 5 | 18.72 ⚠ [14.77–18.97] | 53.43 | 1.96× |
| heap_mpmc | 5 | 9.56 ⚠ [7.75–9.79] | 105 | 1.00× |
| cell_mpmc | 5 | 9.31 ⚠ [7.36–19.78] | 107 | 0.97× |

## W11  E256  producers=1  consumers=4  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 23.04 ⚠ [19.22–23.06] | 43.40 | 2.13× |
| heap_spmc | 5 | 21.64 ⚠ [17.69–21.66] | 46.21 | 2.00× |
| heap_mpmc | 5 | 10.82 ⚠ [8.31–10.88] | 92.38 | 1.00× |
| cell_mpmc | 5 | 9.37 ⚠ [7.66–17.46] | 107 | 0.87× |

## W11  E256  producers=1  consumers=4  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_mpmc | 5 | 3.67 ⚠ [3.49–4.18] | 272 | 1.42× |
| cell_spmc | 5 | 3.62 ⚠ [3.51–4.25] | 277 | 1.40× |
| heap_spmc | 5 | 2.64 ⚠ [2.58–3.19] | 379 | 1.02× |
| heap_mpmc | 5 | 2.58 ⚠ [2.45–3.13] | 387 | 1.00× |

## W11  E256  producers=1  consumers=4  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.26 ⚠ [3.55–4.26] | 235 | 1.05× |
| cell_mpmc | 5 | 4.20 ⚠ [3.49–4.21] | 238 | 1.03× |
| heap_spmc | 5 | 4.10 ⚠ [3.36–4.11] | 244 | 1.01× |
| heap_mpmc | 5 | 4.06 ⚠ [3.37–4.06] | 247 | 1.00× |

## W11  E256  producers=1  consumers=4  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.25 ⚠ [3.60–4.25] | 235 | 1.04× |
| cell_mpmc | 5 | 4.19 ⚠ [3.61–4.19] | 239 | 1.02× |
| heap_spmc | 5 | 4.14 ⚠ [3.52–4.15] | 241 | 1.01× |
| heap_mpmc | 5 | 4.10 ⚠ [3.50–4.11] | 244 | 1.00× |

## W11  E256  producers=1  consumers=8  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 20.75 ⚠ [17.53–24.60] | 48.19 | 25.96× |
| cell_mpmc | 5 | 2.05 [2.01–2.17] | 488 | 2.56× |
| heap_spmc | 5 | 0.81 ⚠ [0.75–0.98] | 1,230 | 1.02× |
| heap_mpmc | 5 | 0.80 ⚠ [0.75–0.97] | 1,251 | 1.00× |

## W11  E256  producers=1  consumers=8  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 18.26 ⚠ [15.89–21.23] | 54.76 | 8.47× |
| heap_spmc | 5 | 11.30 ⚠ [6.68–11.84] | 88.52 | 5.24× |
| cell_mpmc | 5 | 2.37 ⚠ [1.36–2.39] | 423 | 1.10× |
| heap_mpmc | 5 | 2.16 ⚠ [1.55–2.26] | 464 | 1.00× |

## W11  E256  producers=1  consumers=8  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 18.32 ⚠ [16.63–26.12] | 54.59 | 7.54× |
| heap_spmc | 5 | 14.80 ⚠ [14.44–16.90] | 67.55 | 6.09× |
| cell_mpmc | 5 | 2.44 ⚠ [2.01–2.54] | 410 | 1.00× |
| heap_mpmc | 5 | 2.43 ⚠ [2.34–2.91] | 412 | 1.00× |

## W11  E256  producers=1  consumers=8  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 20.93 ⚠ [20.00–24.49] | 47.77 | 20.61× |
| cell_mpmc | 5 | 2.10 ⚠ [1.82–2.15] | 477 | 2.07× |
| heap_mpmc | 5 | 1.02 ⚠ [0.88–1.12] | 985 | 1.00× |
| heap_spmc | 5 | 0.89 ⚠ [0.82–1.04] | 1,118 | 0.88× |

## W11  E256  producers=1  consumers=8  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 26.89 ⚠ [21.03–27.04] | 37.19 | 12.14× |
| heap_spmc | 5 | 13.26 ⚠ [6.84–13.77] | 75.42 | 5.99× |
| cell_mpmc | 5 | 2.42 ⚠ [1.90–2.56] | 412 | 1.09× |
| heap_mpmc | 5 | 2.21 ⚠ [1.85–2.30] | 451 | 1.00× |

## W11  E256  producers=1  consumers=8  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 27.28 ⚠ [20.52–27.54] | 36.65 | 10.81× |
| heap_spmc | 5 | 23.90 ⚠ [18.36–24.02] | 41.85 | 9.47× |
| cell_mpmc | 5 | 2.59 ⚠ [1.87–2.63] | 387 | 1.02× |
| heap_mpmc | 5 | 2.52 ⚠ [1.65–2.94] | 396 | 1.00× |

## W11  E256  producers=1  consumers=8  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 6.37 ⚠ [6.16–7.56] | 157 | 5.53× |
| cell_mpmc | 5 | 6.27 ⚠ [6.14–7.50] | 159 | 5.44× |
| heap_spmc | 5 | 1.16 ⚠ [0.94–1.19] | 863 | 1.01× |
| heap_mpmc | 5 | 1.15 ⚠ [1.10–1.44] | 868 | 1.00× |

## W11  E256  producers=1  consumers=8  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 7.73 ⚠ [6.24–7.73] | 129 | 1.07× |
| cell_mpmc | 5 | 7.66 ⚠ [6.26–7.68] | 131 | 1.06× |
| heap_mpmc | 5 | 7.22 ⚠ [5.86–7.24] | 138 | 1.00× |
| heap_spmc | 5 | 7.21 ⚠ [5.90–7.28] | 139 | 1.00× |

## W11  E256  producers=1  consumers=8  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 7.75 ⚠ [5.94–7.77] | 129 | 1.05× |
| cell_mpmc | 5 | 7.72 ⚠ [5.99–7.73] | 130 | 1.04× |
| heap_mpmc | 5 | 7.41 ⚠ [6.03–7.43] | 135 | 1.00× |
| heap_spmc | 5 | 7.41 ⚠ [6.05–7.44] | 135 | 1.00× |

## W11  E64  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 59.19 ⚠ [46.84–60.30] | 16.89 | 7.46× |
| cell_mpmc | 5 | 13.73 ⚠ [13.36–14.82] | 72.84 | 1.73× |
| heap_spmc | 5 | 11.94 ⚠ [10.51–12.05] | 83.76 | 1.51× |
| heap_mpmc | 5 | 7.93 ⚠ [7.21–8.23] | 126 | 1.00× |

## W11  E64  producers=1  consumers=1  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 55.42 ⚠ [46.63–60.07] | 18.05 | 4.26× |
| heap_spmc | 5 | 43.30 ⚠ [33.72–48.30] | 23.09 | 3.33× |
| cell_mpmc | 5 | 13.62 ⚠ [12.59–24.80] | 73.41 | 1.05× |
| heap_mpmc | 5 | 13.00 ⚠ [12.58–14.59] | 76.93 | 1.00× |

## W11  E64  producers=1  consumers=1  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 78.26 ⚠ [68.41–80.91] | 12.78 | 5.02× |
| cell_spmc | 5 | 60.18 ⚠ [45.91–60.77] | 16.62 | 3.86× |
| heap_mpmc | 5 | 15.59 ⚠ [13.32–15.67] | 64.16 | 1.00× |
| cell_mpmc | 5 | 14.38 ⚠ [12.00–14.60] | 69.56 | 0.92× |

## W11  E64  producers=1  consumers=1  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 9.52 ⚠ [7.81–9.54] | 105 | 1.11× |
| cell_mpmc | 5 | 9.11 ⚠ [7.89–9.12] | 110 | 1.06× |
| heap_spmc | 5 | 9.06 ⚠ [6.68–9.31] | 110 | 1.05× |
| heap_mpmc | 5 | 8.59 ⚠ [6.79–8.73] | 116 | 1.00× |

## W11  E64  producers=1  consumers=1  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 10.61 ⚠ [7.39–11.47] | 94.24 | 1.06× |
| heap_mpmc | 5 | 10.05 ⚠ [7.26–11.43] | 99.49 | 1.00× |
| cell_spmc | 5 | 8.91 ⚠ [7.77–9.52] | 112 | 0.89× |
| cell_mpmc | 5 | 8.53 ⚠ [7.70–9.13] | 117 | 0.85× |

## W11  E64  producers=1  consumers=1  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 11.90 ⚠ [7.65–11.91] | 84.05 | 1.01× |
| heap_mpmc | 5 | 11.83 ⚠ [8.13–11.87] | 84.51 | 1.00× |
| cell_spmc | 5 | 9.54 ⚠ [7.66–9.55] | 105 | 0.81× |
| cell_mpmc | 5 | 9.13 ⚠ [7.48–9.13] | 110 | 0.77× |

## W11  E64  producers=1  consumers=1  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_spmc | 5 | 1.13 ⚠ [0.97–1.13] | 886 | 1.00× |
| heap_mpmc | 5 | 1.13 ⚠ [0.97–1.13] | 888 | 1.00× |
| cell_spmc | 5 | 1.11 ⚠ [0.94–1.12] | 897 | 0.99× |
| cell_mpmc | 5 | 1.11 ⚠ [0.94–1.11] | 902 | 0.98× |

## W11  E64  producers=1  consumers=1  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 1.10 ⚠ [0.97–1.12] | 905 | 1.04× |
| cell_mpmc | 5 | 1.09 ⚠ [0.95–1.11] | 915 | 1.03× |
| heap_spmc | 5 | 1.09 ⚠ [0.94–1.13] | 915 | 1.03× |
| heap_mpmc | 5 | 1.07 ⚠ [0.94–1.13] | 938 | 1.00× |

## W11  E64  producers=1  consumers=1  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| heap_mpmc | 5 | 1.13 ⚠ [0.95–1.14] | 881 | 1.00× |
| heap_spmc | 5 | 1.13 ⚠ [0.97–1.14] | 882 | 1.00× |
| cell_spmc | 5 | 1.12 ⚠ [0.95–1.12] | 896 | 0.98× |
| cell_mpmc | 5 | 1.11 ⚠ [0.97–1.11] | 901 | 0.98× |

## W11  E64  producers=1  consumers=2  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 35.57 ⚠ [32.14–38.13] | 28.12 | 5.24× |
| cell_mpmc | 5 | 10.29 ⚠ [8.92–10.60] | 97.21 | 1.52× |
| heap_spmc | 5 | 8.39 [8.37–8.59] | 119 | 1.24× |
| heap_mpmc | 5 | 6.79 ⚠ [6.15–6.87] | 147 | 1.00× |

## W11  E64  producers=1  consumers=2  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 35.11 ⚠ [32.45–38.00] | 28.48 | 3.18× |
| heap_spmc | 5 | 34.51 ⚠ [32.29–35.21] | 28.97 | 3.12× |
| heap_mpmc | 5 | 11.05 ⚠ [10.20–11.41] | 90.46 | 1.00× |
| cell_mpmc | 5 | 10.49 ⚠ [9.34–10.81] | 95.33 | 0.95× |

## W11  E64  producers=1  consumers=2  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 34.01 ⚠ [31.04–35.01] | 29.40 | 1.62× |
| heap_spmc | 5 | 22.46 [21.79–22.60] | 44.52 | 1.07× |
| heap_mpmc | 5 | 21.02 ⚠ [12.06–23.78] | 47.56 | 1.00× |
| cell_mpmc | 5 | 11.03 ⚠ [9.39–17.16] | 90.70 | 0.52× |

## W11  E64  producers=1  consumers=2  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 15.12 ⚠ [13.26–15.19] | 66.14 | 1.97× |
| cell_mpmc | 5 | 14.10 ⚠ [13.09–14.22] | 70.90 | 1.83× |
| heap_spmc | 5 | 11.78 ⚠ [9.83–11.98] | 84.86 | 1.53× |
| heap_mpmc | 5 | 7.69 ⚠ [7.40–9.79] | 130 | 1.00× |

## W11  E64  producers=1  consumers=2  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 15.22 ⚠ [13.22–15.23] | 65.69 | 1.12× |
| heap_spmc | 5 | 14.66 ⚠ [11.82–14.83] | 68.20 | 1.08× |
| cell_mpmc | 5 | 14.09 [13.36–14.59] | 70.99 | 1.04× |
| heap_mpmc | 5 | 13.57 ⚠ [11.58–13.61] | 73.70 | 1.00× |

## W11  E64  producers=1  consumers=2  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 14.71 ⚠ [12.72–15.32] | 68.00 | 1.12× |
| cell_mpmc | 5 | 14.08 ⚠ [12.47–14.38] | 71.03 | 1.07× |
| heap_spmc | 5 | 13.93 ⚠ [10.89–13.95] | 71.77 | 1.06× |
| heap_mpmc | 5 | 13.14 ⚠ [10.58–13.19] | 76.11 | 1.00× |

## W11  E64  producers=1  consumers=2  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 2.18 ⚠ [1.91–2.20] | 459 | 1.04× |
| cell_mpmc | 5 | 2.15 ⚠ [1.89–2.17] | 465 | 1.02× |
| heap_spmc | 5 | 2.11 ⚠ [1.81–2.11] | 473 | 1.00× |
| heap_mpmc | 5 | 2.10 ⚠ [1.81–2.11] | 475 | 1.00× |

## W11  E64  producers=1  consumers=2  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 2.20 ⚠ [1.88–2.21] | 454 | 1.03× |
| cell_mpmc | 5 | 2.17 ⚠ [1.84–2.17] | 461 | 1.02× |
| heap_spmc | 5 | 2.16 ⚠ [1.78–2.16] | 463 | 1.01× |
| heap_mpmc | 5 | 2.14 ⚠ [1.78–2.14] | 468 | 1.00× |

## W11  E64  producers=1  consumers=2  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 2.20 ⚠ [1.88–2.21] | 454 | 1.04× |
| cell_mpmc | 5 | 2.17 ⚠ [1.88–2.17] | 462 | 1.02× |
| heap_spmc | 5 | 2.14 ⚠ [1.81–2.15] | 467 | 1.01× |
| heap_mpmc | 5 | 2.13 ⚠ [1.74–2.13] | 470 | 1.00× |

## W11  E64  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 34.27 ⚠ [30.67–35.71] | 29.18 | 5.55× |
| cell_mpmc | 5 | 9.20 ⚠ [7.94–9.44] | 109 | 1.49× |
| heap_spmc | 5 | 6.97 ⚠ [6.09–7.29] | 143 | 1.13× |
| heap_mpmc | 5 | 6.17 ⚠ [5.39–6.36] | 162 | 1.00× |

## W11  E64  producers=1  consumers=4  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 36.16 ⚠ [30.89–40.62] | 27.65 | 3.73× |
| heap_spmc | 5 | 25.63 ⚠ [22.91–27.18] | 39.01 | 2.64× |
| heap_mpmc | 5 | 9.69 ⚠ [8.10–9.72] | 103 | 1.00× |
| cell_mpmc | 5 | 9.47 ⚠ [8.05–9.59] | 106 | 0.98× |

## W11  E64  producers=1  consumers=4  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 33.43 ⚠ [31.65–37.01] | 29.91 | 3.34× |
| heap_spmc | 5 | 21.48 ⚠ [20.02–21.94] | 46.54 | 2.14× |
| heap_mpmc | 5 | 10.02 ⚠ [8.70–10.18] | 99.79 | 1.00× |
| cell_mpmc | 5 | 9.51 ⚠ [8.07–9.57] | 105 | 0.95× |

## W11  E64  producers=1  consumers=4  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 25.42 ⚠ [21.36–25.72] | 39.33 | 3.80× |
| cell_mpmc | 5 | 18.35 ⚠ [9.60–20.18] | 54.51 | 2.74× |
| heap_spmc | 5 | 9.03 ⚠ [8.75–10.04] | 111 | 1.35× |
| heap_mpmc | 5 | 6.70 ⚠ [5.89–6.91] | 149 | 1.00× |

## W11  E64  producers=1  consumers=4  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 25.64 ⚠ [20.76–25.74] | 39.00 | 2.44× |
| heap_spmc | 5 | 23.21 ⚠ [19.30–23.46] | 43.09 | 2.21× |
| heap_mpmc | 5 | 10.50 ⚠ [10.45–12.56] | 95.28 | 1.00× |
| cell_mpmc | 5 | 9.69 ⚠ [9.67–22.06] | 103 | 0.92× |

## W11  E64  producers=1  consumers=4  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 25.32 ⚠ [21.21–25.55] | 39.50 | 2.29× |
| heap_spmc | 5 | 24.40 ⚠ [20.09–24.50] | 40.98 | 2.21× |
| heap_mpmc | 5 | 11.04 ⚠ [10.98–17.12] | 90.62 | 1.00× |
| cell_mpmc | 5 | 9.70 ⚠ [8.27–9.80] | 103 | 0.88× |

## W11  E64  producers=1  consumers=4  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.35 ⚠ [3.75–4.36] | 230 | 1.05× |
| cell_mpmc | 5 | 4.32 ⚠ [3.72–4.33] | 232 | 1.04× |
| heap_spmc | 5 | 4.19 ⚠ [3.60–4.20] | 239 | 1.01× |
| heap_mpmc | 5 | 4.13 ⚠ [3.56–4.15] | 242 | 1.00× |

## W11  E64  producers=1  consumers=4  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.36 ⚠ [3.67–4.37] | 229 | 1.04× |
| cell_mpmc | 5 | 4.31 ⚠ [3.62–4.32] | 232 | 1.03× |
| heap_spmc | 5 | 4.22 ⚠ [3.55–4.26] | 237 | 1.01× |
| heap_mpmc | 5 | 4.20 ⚠ [3.55–4.21] | 238 | 1.00× |

## W11  E64  producers=1  consumers=4  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 4.36 ⚠ [3.66–4.37] | 229 | 1.04× |
| cell_mpmc | 5 | 4.30 ⚠ [3.69–4.31] | 232 | 1.02× |
| heap_spmc | 5 | 4.24 ⚠ [3.60–4.25] | 236 | 1.01× |
| heap_mpmc | 5 | 4.20 ⚠ [3.59–4.20] | 238 | 1.00× |

## W11  E64  producers=1  consumers=8  work=0  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 29.58 ⚠ [22.69–30.49] | 33.81 | 15.13× |
| heap_spmc | 5 | 3.19 ⚠ [2.79–3.63] | 314 | 1.63× |
| cell_mpmc | 5 | 2.07 ⚠ [1.96–2.47] | 483 | 1.06× |
| heap_mpmc | 5 | 1.95 ⚠ [1.65–2.03] | 512 | 1.00× |

## W11  E64  producers=1  consumers=8  work=0  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 28.64 ⚠ [22.70–30.91] | 34.92 | 12.04× |
| heap_spmc | 5 | 12.94 ⚠ [11.97–13.69] | 77.25 | 5.44× |
| heap_mpmc | 5 | 2.38 ⚠ [1.45–2.54] | 420 | 1.00× |
| cell_mpmc | 5 | 2.14 ⚠ [1.95–2.49] | 468 | 0.90× |

## W11  E64  producers=1  consumers=8  work=0  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 28.63 ⚠ [22.54–29.62] | 34.93 | 11.20× |
| heap_spmc | 5 | 12.12 [12.02–12.96] | 82.50 | 4.74× |
| heap_mpmc | 5 | 2.56 ⚠ [2.31–2.64] | 391 | 1.00× |
| cell_mpmc | 5 | 2.20 ⚠ [1.70–2.60] | 455 | 0.86× |

## W11  E64  producers=1  consumers=8  work=100  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 30.40 ⚠ [22.85–32.40] | 32.90 | 15.00× |
| heap_spmc | 5 | 4.59 ⚠ [3.90–5.69] | 218 | 2.27× |
| cell_mpmc | 5 | 2.40 ⚠ [1.69–2.65] | 417 | 1.18× |
| heap_mpmc | 5 | 2.03 ⚠ [1.66–2.21] | 493 | 1.00× |

## W11  E64  producers=1  consumers=8  work=100  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 25.91 ⚠ [21.91–32.65] | 38.60 | 10.49× |
| heap_spmc | 5 | 22.87 ⚠ [19.98–23.70] | 43.73 | 9.26× |
| cell_mpmc | 5 | 2.51 ⚠ [1.59–2.64] | 398 | 1.02× |
| heap_mpmc | 5 | 2.47 ⚠ [1.57–2.64] | 405 | 1.00× |

## W11  E64  producers=1  consumers=8  work=100  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 31.68 ⚠ [22.29–32.96] | 31.56 | 12.05× |
| heap_spmc | 5 | 25.54 ⚠ [19.95–25.93] | 39.15 | 9.71× |
| heap_mpmc | 5 | 2.63 ⚠ [2.06–2.73] | 380 | 1.00× |
| cell_mpmc | 5 | 2.56 ⚠ [2.06–2.70] | 390 | 0.97× |

## W11  E64  producers=1  consumers=8  work=1000  alloc=glibc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 7.33 ⚠ [6.59–7.94] | 136 | 1.21× |
| cell_mpmc | 5 | 7.03 ⚠ [6.65–7.93] | 142 | 1.16× |
| heap_spmc | 5 | 6.93 ⚠ [5.95–7.05] | 144 | 1.14× |
| heap_mpmc | 5 | 6.07 ⚠ [2.28–7.21] | 165 | 1.00× |

## W11  E64  producers=1  consumers=8  work=1000  alloc=jemalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 7.82 ⚠ [6.19–7.94] | 128 | 1.07× |
| cell_mpmc | 5 | 7.56 ⚠ [6.24–7.92] | 132 | 1.03× |
| heap_spmc | 5 | 7.37 ⚠ [6.23–7.62] | 136 | 1.01× |
| heap_mpmc | 5 | 7.32 ⚠ [5.97–7.52] | 137 | 1.00× |

## W11  E64  producers=1  consumers=8  work=1000  alloc=snmalloc

| variant | n | Mops/s | ns/op | Mops/s vs heap_mpmc |
|---|---|---|---|---|
| cell_spmc | 5 | 7.90 ⚠ [6.19–7.98] | 127 | 1.06× |
| cell_mpmc | 5 | 7.72 ⚠ [6.31–7.95] | 130 | 1.03× |
| heap_spmc | 5 | 7.63 ⚠ [6.24–7.68] | 131 | 1.02× |
| heap_mpmc | 5 | 7.48 ⚠ [6.32–7.64] | 134 | 1.00× |

## W11  [mechanism pass]  E16  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 540 | 167 | 7.03 | 0.31 | 3.50 |
| cell_spmc | 1 | 0.00 | 0.00 | 54.65 | 113 | 0.25 | 2.07 | 4.50 |
| heap_mpmc | 1 | 0.00 | 0.00 | 557 | 238 | 6.77 | 0.43 | 4.00 |
| heap_spmc | 1 | 0.00 | 0.00 | 68.17 | 158 | 0.04 | 2.32 | 4.69 |

## W11  [mechanism pass]  E16  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 2,049 | 301 | 14.95 | 0.15 | 5.70 |
| cell_spmc | 1 | 0.00 | 0.00 | 589 | 168 | 1.96 | 0.29 | 6.15 |
| heap_mpmc | 1 | 0.00 | 0.00 | 2,026 | 367 | 14.84 | 0.18 | 6.00 |
| heap_spmc | 1 | 0.00 | 0.00 | 829 | 230 | 5.44 | 0.28 | 6.18 |

## W11  [mechanism pass]  E256  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 367 | 371 | 0.49 | 1.01 | 9.40 |
| cell_spmc | 1 | 0.00 | 0.00 | 325 | 323 | 0.19 | 0.99 | 9.39 |
| heap_mpmc | 1 | 1.00 | 1.00 | 925 | 932 | 13.68 | 1.01 | 6.50 |
| heap_spmc | 1 | 1.00 | 1.00 | 685 | 852 | 9.80 | 1.24 | 6.50 |

## W11  [mechanism pass]  E256  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 2,374 | 517 | 26.33 | 0.22 | 14.25 |
| cell_spmc | 1 | 0.00 | 0.00 | 557 | 329 | 9.47 | 0.59 | 16.53 |
| heap_mpmc | 1 | 1.00 | 1.00 | 6,910 | 2,162 | 45.87 | 0.31 | 6.00 |
| heap_spmc | 1 | 1.00 | 1.00 | 5,851 | 2,012 | 46.67 | 0.34 | 5.50 |

## W11  [mechanism pass]  E64  producers=1  consumers=1  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 548 | 201 | 8.13 | 0.37 | 4.80 |
| cell_spmc | 1 | 0.00 | 0.00 | 135 | 151 | 0.30 | 1.12 | 5.47 |
| heap_mpmc | 1 | 1.00 | 1.00 | 1,415 | 598 | 21.30 | 0.42 | 3.50 |
| heap_spmc | 1 | 1.00 | 1.00 | 1,152 | 528 | 19.11 | 0.46 | 4.00 |

## W11  [mechanism pass]  E64  producers=1  consumers=4  work=0  alloc=glibc

| variant | n | allocs/elem | frees/elem | cycles/elem | instr/elem | cache misses/elem | IPC | peak RSS MiB |
|---|---|---|---|---|---|---|---|---|
| cell_mpmc | 1 | 0.00 | 0.00 | 2,181 | 332 | 19.18 | 0.15 | 7.46 |
| cell_spmc | 1 | 0.00 | 0.00 | 556 | 188 | 2.72 | 0.34 | 8.36 |
| heap_mpmc | 1 | 1.00 | 1.00 | 4,246 | 982 | 33.34 | 0.23 | 6.25 |
| heap_spmc | 1 | 1.00 | 1.00 | 4,151 | 775 | 48.49 | 0.19 | 5.00 |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
