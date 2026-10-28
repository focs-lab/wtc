# eval-ablation  (2026-10-28T09:56:48Z)

AMD EPYC 9115 16-Core Processor, kernel 6.8.0-137-generic, allowed cpus 16-31,48-63, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=0;`, git 

Control (shim_generic E64, median of 3 and 3): open 15.93, close 15.89 Mops/s, drift 0.2% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 0.998, rep 2 0.998, rep 3 0.999, rep 4 1.000 (n=240) — no trend.  Widest single row: mpmc_share in W10  E8  wait=spin  alloc=glibc, 81% across repetitions.

## W10  E256  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 94.69 ⚠ [87.05–95.60] | 10.56 |
| spsc_cell | 5 | 87.03 ⚠ [86.55–93.81] | 11.49 |
| mc_cell | 5 | 28.62 [28.37–28.64] | 34.94 |
| mpmc_share | 5 | 16.54 [16.14–16.88] | 60.47 |
| mpmc_cell | 5 | 14.02 [13.48–14.13] | 71.32 |
| spsc_xsp | 5 | 9.35 ⚠ [8.66–9.98] | 107 |
| mc_xsp | 5 | 6.74 [6.71–6.91] | 148 |
| mpmc_xsp | 5 | 6.14 [6.13–6.31] | 163 |

## W10  E256  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 107 ⚠ [97.51–109] | 9.31 |
| spsc_cell | 5 | 90.21 [89.35–91.11] | 11.08 |
| spsc_xsp | 5 | 28.39 [28.08–28.56] | 35.23 |
| mc_cell | 5 | 27.90 [27.84–28.00] | 35.84 |
| mpmc_xsp | 5 | 16.45 [16.18–16.65] | 60.78 |
| mpmc_share | 5 | 15.13 ⚠ [14.36–19.95] | 66.10 |
| mc_xsp | 5 | 14.52 ⚠ [14.46–16.09] | 68.86 |
| mpmc_cell | 5 | 14.00 [13.80–14.20] | 71.41 |

## W10  E256  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 105 ⚠ [92.77–105] | 9.55 |
| spsc_cell | 5 | 94.15 ⚠ [89.51–105] | 10.62 |
| spsc_xsp | 5 | 32.88 [32.68–33.32] | 30.41 |
| mc_cell | 5 | 29.03 [28.80–29.20] | 34.45 |
| mc_xsp | 5 | 18.74 ⚠ [17.11–18.92] | 53.35 |
| mpmc_share | 5 | 14.28 ⚠ [13.69–15.68] | 70.02 |
| mpmc_cell | 5 | 13.62 [13.46–14.03] | 73.40 |
| mpmc_xsp | 5 | 8.22 [8.14–8.74] | 122 |

## W10  E256  wait=spin  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 101 [97.30–101] | 9.92 |
| spsc_cell | 5 | 94.33 ⚠ [89.58–109] | 10.60 |
| spsc_xsp | 5 | 89.91 ⚠ [82.12–91.29] | 11.12 |
| mc_cell | 5 | 27.84 [27.78–27.89] | 35.91 |
| mc_xsp | 5 | 25.10 [24.15–25.15] | 39.84 |
| mpmc_xsp | 5 | 16.03 [15.90–16.39] | 62.39 |
| mpmc_share | 5 | 14.25 [13.61–14.68] | 70.20 |
| mpmc_cell | 5 | 13.54 [13.51–14.22] | 73.86 |

## W10  E256  wait=spin  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 119 ⚠ [94.05–127] | 8.39 |
| spsc_cell | 5 | 98.63 ⚠ [94.33–109] | 10.14 |
| mc_cell | 5 | 28.02 [27.99–28.10] | 35.68 |
| spsc_xsp | 5 | 20.22 ⚠ [17.71–20.81] | 49.45 |
| mpmc_share | 5 | 14.18 [14.02–14.23] | 70.53 |
| mpmc_xsp | 5 | 14.08 [13.94–14.22] | 71.01 |
| mpmc_cell | 5 | 13.87 [13.70–14.27] | 72.10 |
| mc_xsp | 5 | 12.28 [12.18–12.45] | 81.44 |

## W10  E256  wait=spin  (smt)  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 249 [246–252] | 4.01 |
| spsc_share | 5 | 56.30 [55.22–56.55] | 17.76 |
| mc_cell | 5 | 28.17 [28.07–28.25] | 35.50 |
| mpmc_share | 5 | 26.04 [25.94–27.24] | 38.40 |
| mpmc_cell | 5 | 25.65 [24.88–26.16] | 38.99 |
| spsc_xsp | 5 | 9.35 [9.18–9.50] | 107 |
| mc_xsp | 5 | 8.56 [8.49–8.70] | 117 |
| mpmc_xsp | 5 | 7.21 [7.19–7.33] | 139 |

## W10  E256  wait=spin  (smt)  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 249 ⚠ [232–252] | 4.02 |
| spsc_share | 5 | 73.17 [71.77–74.46] | 13.67 |
| mpmc_share | 5 | 38.53 [38.24–39.00] | 25.95 |
| mpmc_cell | 5 | 31.32 [31.11–32.19] | 31.93 |
| mpmc_xsp | 5 | 26.81 [26.45–26.97] | 37.29 |
| mc_cell | 5 | 26.61 [25.84–26.72] | 37.58 |
| spsc_xsp | 5 | 20.91 [20.76–21.32] | 47.83 |
| mc_xsp | 5 | 14.64 ⚠ [13.18–14.89] | 68.31 |

## W10  E256  wait=spin  (smt)  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 249 [239–253] | 4.02 |
| spsc_share | 5 | 55.98 [54.96–56.77] | 17.86 |
| mpmc_share | 5 | 40.19 [39.88–40.30] | 24.88 |
| spsc_xsp | 5 | 38.70 [38.54–39.05] | 25.84 |
| mpmc_cell | 5 | 30.78 ⚠ [29.89–32.65] | 32.49 |
| mc_cell | 5 | 27.29 [26.79–28.44] | 36.65 |
| mc_xsp | 5 | 19.34 [19.24–19.82] | 51.72 |
| mpmc_xsp | 5 | 19.19 ⚠ [18.35–20.75] | 52.12 |

## W10  E256  wait=spin  (smt)  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 245 [241–251] | 4.08 |
| spsc_share | 5 | 80.06 [76.69–81.40] | 12.49 |
| spsc_xsp | 5 | 57.65 [57.39–58.47] | 17.35 |
| mpmc_xsp | 5 | 34.81 [33.78–36.04] | 28.73 |
| mpmc_share | 5 | 32.56 [31.13–33.34] | 30.71 |
| mpmc_cell | 5 | 30.51 ⚠ [28.60–31.71] | 32.78 |
| mc_cell | 5 | 25.95 [25.83–26.11] | 38.53 |
| mc_xsp | 5 | 22.94 [22.35–23.09] | 43.59 |

## W10  E256  wait=spin  (smt)  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 247 [241–251] | 4.05 |
| spsc_share | 5 | 84.17 [81.67–84.64] | 11.88 |
| mpmc_share | 5 | 35.11 [34.31–35.58] | 28.48 |
| mpmc_cell | 5 | 32.28 [31.23–32.65] | 30.98 |
| mpmc_xsp | 5 | 31.17 ⚠ [30.34–33.08] | 32.08 |
| spsc_xsp | 5 | 28.53 [28.28–28.89] | 35.05 |
| mc_cell | 5 | 27.25 ⚠ [25.15–27.56] | 36.69 |
| mc_xsp | 5 | 17.13 [17.06–17.18] | 58.38 |

## W10  E64  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 281 [277–293] | 3.56 |
| spsc_share | 5 | 119 [115–120] | 8.44 |
| mc_cell | 5 | 34.27 [34.25–34.31] | 29.18 |
| spsc_xsp | 5 | 17.06 [16.82–17.23] | 58.63 |
| mpmc_share | 5 | 16.85 [16.64–17.38] | 59.36 |
| mpmc_cell | 5 | 15.82 [15.58–15.88] | 63.22 |
| mc_xsp | 5 | 11.64 [11.61–11.67] | 85.91 |
| mpmc_xsp | 5 | 7.46 [7.41–7.58] | 134 |

## W10  E64  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 295 [283–305] | 3.39 |
| spsc_share | 5 | 150 [150–150] | 6.65 |
| spsc_xsp | 5 | 31.50 [31.45–31.63] | 31.75 |
| mc_cell | 5 | 31.22 [29.74–31.32] | 32.03 |
| mpmc_xsp | 5 | 18.30 [18.03–18.91] | 54.63 |
| mc_xsp | 5 | 16.93 [16.25–17.06] | 59.07 |
| mpmc_share | 5 | 16.20 [15.87–16.81] | 61.74 |
| mpmc_cell | 5 | 16.05 [16.00–16.14] | 62.31 |

## W10  E64  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 290 [283–305] | 3.45 |
| spsc_share | 5 | 124 ⚠ [107–125] | 8.04 |
| spsc_xsp | 5 | 63.76 [62.99–63.94] | 15.68 |
| mc_cell | 5 | 35.59 [34.94–36.01] | 28.10 |
| mc_xsp | 5 | 21.93 ⚠ [19.34–22.18] | 45.61 |
| mpmc_share | 5 | 15.93 [15.80–16.01] | 62.77 |
| mpmc_cell | 5 | 15.78 [15.75–16.01] | 63.37 |
| mpmc_xsp | 5 | 8.08 [8.01–8.30] | 124 |

## W10  E64  wait=spin  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 302 ⚠ [282–315] | 3.31 |
| spsc_share | 5 | 159 [157–159] | 6.29 |
| spsc_xsp | 5 | 134 [134–135] | 7.44 |
| mc_cell | 5 | 35.31 [34.38–35.37] | 28.32 |
| mc_xsp | 5 | 26.60 [25.57–26.70] | 37.59 |
| mpmc_share | 5 | 16.00 [15.82–16.37] | 62.50 |
| mpmc_cell | 5 | 15.92 [15.80–15.99] | 62.81 |
| mpmc_xsp | 5 | 15.24 [14.99–15.93] | 65.61 |

## W10  E64  wait=spin  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 289 ⚠ [272–301] | 3.46 |
| spsc_share | 5 | 157 [157–157] | 6.36 |
| mc_cell | 5 | 31.58 ⚠ [28.16–32.80] | 31.67 |
| spsc_xsp | 5 | 26.85 [26.58–27.05] | 37.24 |
| mpmc_cell | 5 | 15.91 [15.68–16.03] | 62.85 |
| mpmc_share | 5 | 15.64 [15.52–15.93] | 63.95 |
| mc_xsp | 5 | 15.23 [14.71–15.27] | 65.66 |
| mpmc_xsp | 5 | 14.16 [13.90–14.30] | 70.63 |

## W10  E64  wait=spin  (smt)  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 435 [435–436] | 2.30 |
| spsc_share | 5 | 65.76 [65.52–65.99] | 15.21 |
| mpmc_share | 5 | 38.67 ⚠ [35.73–40.39] | 25.86 |
| mpmc_cell | 5 | 32.16 ⚠ [29.05–34.55] | 31.10 |
| mc_cell | 5 | 30.58 [30.55–30.78] | 32.70 |
| spsc_xsp | 5 | 22.68 [21.69–22.87] | 44.10 |
| mc_xsp | 5 | 15.53 [15.43–15.59] | 64.39 |
| mpmc_xsp | 5 | 14.51 [14.40–14.68] | 68.92 |

## W10  E64  wait=spin  (smt)  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 435 [434–435] | 2.30 |
| spsc_share | 5 | 100 [99.75–101] | 9.99 |
| mpmc_share | 5 | 39.71 [38.71–40.25] | 25.18 |
| mpmc_cell | 5 | 34.07 [33.61–35.26] | 29.35 |
| mpmc_xsp | 5 | 32.96 [32.41–33.87] | 30.34 |
| mc_cell | 5 | 28.72 [28.52–28.94] | 34.82 |
| spsc_xsp | 5 | 23.65 [23.34–23.97] | 42.28 |
| mc_xsp | 5 | 15.65 [15.59–15.71] | 63.90 |

## W10  E64  wait=spin  (smt)  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 435 [433–436] | 2.30 |
| spsc_share | 5 | 75.37 [73.81–75.44] | 13.27 |
| spsc_xsp | 5 | 50.55 [49.63–50.74] | 19.78 |
| mpmc_share | 5 | 38.39 ⚠ [38.22–43.88] | 26.05 |
| mpmc_cell | 5 | 34.21 ⚠ [31.58–36.11] | 29.23 |
| mc_cell | 5 | 29.13 ⚠ [28.63–31.00] | 34.33 |
| mpmc_xsp | 5 | 22.58 ⚠ [20.06–26.33] | 44.29 |
| mc_xsp | 5 | 21.69 [21.01–21.93] | 46.11 |

## W10  E64  wait=spin  (smt)  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 434 [433–435] | 2.30 |
| spsc_share | 5 | 123 [122–123] | 8.15 |
| spsc_xsp | 5 | 80.77 [79.44–81.37] | 12.38 |
| mpmc_share | 5 | 36.80 [36.34–37.44] | 27.18 |
| mpmc_cell | 5 | 33.88 [32.33–34.06] | 29.52 |
| mpmc_xsp | 5 | 33.65 [32.85–34.27] | 29.71 |
| mc_cell | 5 | 28.52 [28.44–28.69] | 35.07 |
| mc_xsp | 5 | 24.31 [24.25–24.83] | 41.14 |

## W10  E64  wait=spin  (smt)  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 435 [434–435] | 2.30 |
| spsc_share | 5 | 111 [111–112] | 8.99 |
| mpmc_share | 5 | 39.45 [37.60–39.99] | 25.35 |
| spsc_xsp | 5 | 36.22 [36.17–36.36] | 27.61 |
| mpmc_xsp | 5 | 33.92 ⚠ [31.86–34.82] | 29.48 |
| mpmc_cell | 5 | 33.51 ⚠ [29.57–37.23] | 29.84 |
| mc_cell | 5 | 28.86 [28.83–28.90] | 34.65 |
| mc_xsp | 5 | 18.55 [18.48–18.66] | 53.92 |

## W10  E8  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 769 ⚠ [625–867] | 1.30 |
| spsc_share | 5 | 114 [113–114] | 8.78 |
| mc_cell | 5 | 36.88 [36.19–36.97] | 27.11 |
| mpmc_share | 5 | 19.49 ⚠ [18.92–34.33] | 51.31 |
| spsc_xsp | 5 | 18.18 [18.14–18.30] | 55.02 |
| mpmc_cell | 5 | 16.26 [15.93–16.44] | 61.49 |
| mc_xsp | 5 | 12.07 [12.03–12.30] | 82.85 |
| mpmc_xsp | 5 | 7.39 [7.32–7.52] | 135 |

## W10  E8  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 881 [864–908] | 1.13 |
| spsc_share | 5 | 178 [177–178] | 5.63 |
| spsc_xsp | 5 | 31.19 [31.02–31.31] | 32.06 |
| mc_cell | 5 | 30.56 [29.24–30.69] | 32.72 |
| mpmc_xsp | 5 | 17.10 [16.97–17.69] | 58.49 |
| mc_xsp | 5 | 17.06 [16.94–17.22] | 58.60 |
| mpmc_share | 5 | 16.47 [16.41–16.73] | 60.73 |
| mpmc_cell | 5 | 16.35 [16.21–16.70] | 61.16 |

## W10  E8  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 863 ⚠ [814–912] | 1.16 |
| spsc_share | 5 | 145 [144–145] | 6.91 |
| spsc_xsp | 5 | 68.85 ⚠ [60.85–73.35] | 14.52 |
| mc_cell | 5 | 34.08 [33.93–34.09] | 29.35 |
| mc_xsp | 5 | 23.70 [23.17–24.22] | 42.20 |
| mpmc_cell | 5 | 16.35 [16.20–16.63] | 61.16 |
| mpmc_share | 5 | 16.23 [16.20–16.63] | 61.60 |
| mpmc_xsp | 5 | 8.54 [8.35–8.61] | 117 |

## W10  E8  wait=spin  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 873 ⚠ [775–901] | 1.15 |
| spsc_share | 5 | 214 [213–214] | 4.68 |
| spsc_xsp | 5 | 149 ⚠ [139–155] | 6.71 |
| mc_cell | 5 | 37.74 [37.63–37.96] | 26.50 |
| mc_xsp | 5 | 29.56 [28.91–29.89] | 33.83 |
| mpmc_share | 5 | 16.68 [16.43–17.07] | 59.94 |
| mpmc_cell | 5 | 16.44 [16.38–16.57] | 60.82 |
| mpmc_xsp | 5 | 13.70 [13.53–13.83] | 72.97 |

## W10  E8  wait=spin  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 878 [872–907] | 1.14 |
| spsc_share | 5 | 195 [195–195] | 5.13 |
| spsc_xsp | 5 | 31.17 [30.80–31.40] | 32.08 |
| mc_cell | 5 | 30.72 [29.35–30.86] | 32.56 |
| mc_xsp | 5 | 16.70 [16.66–16.87] | 59.87 |
| mpmc_cell | 5 | 16.39 [16.23–16.61] | 61.01 |
| mpmc_share | 5 | 16.38 [16.30–16.61] | 61.04 |
| mpmc_xsp | 5 | 14.38 [14.27–14.59] | 69.54 |

## W10  E8  wait=spin  (smt)  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 524 ⚠ [305–536] | 1.91 |
| spsc_share | 5 | 64.46 [64.00–64.71] | 15.51 |
| mpmc_share | 5 | 42.90 [42.32–42.99] | 23.31 |
| mpmc_cell | 5 | 34.28 [33.02–35.05] | 29.17 |
| mc_cell | 5 | 32.58 [32.38–32.69] | 30.69 |
| spsc_xsp | 5 | 22.55 [22.52–22.84] | 44.35 |
| mc_xsp | 5 | 15.86 [15.76–15.92] | 63.07 |
| mpmc_xsp | 5 | 14.12 [13.80–14.42] | 70.81 |

## W10  E8  wait=spin  (smt)  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 529 [513–530] | 1.89 |
| spsc_share | 5 | 101 [99.41–101] | 9.92 |
| mpmc_share | 5 | 38.99 [38.40–39.15] | 25.65 |
| mpmc_xsp | 5 | 34.33 [34.31–34.78] | 29.13 |
| mpmc_cell | 5 | 33.67 [33.17–33.96] | 29.70 |
| mc_cell | 5 | 29.96 [29.84–30.22] | 33.38 |
| spsc_xsp | 5 | 24.00 [23.36–24.02] | 41.67 |
| mc_xsp | 5 | 15.54 [15.48–15.57] | 64.37 |

## W10  E8  wait=spin  (smt)  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 533 [526–535] | 1.88 |
| spsc_share | 5 | 78.34 [77.65–79.55] | 12.77 |
| spsc_xsp | 5 | 57.34 [56.47–58.38] | 17.44 |
| mpmc_share | 5 | 36.88 [36.22–37.21] | 27.12 |
| mpmc_cell | 5 | 33.22 [32.87–34.28] | 30.10 |
| mc_cell | 5 | 30.34 ⚠ [30.29–32.93] | 32.96 |
| mpmc_xsp | 5 | 29.36 ⚠ [25.80–33.39] | 34.06 |
| mc_xsp | 5 | 22.20 [21.75–23.08] | 45.04 |

## W10  E8  wait=spin  (smt)  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 532 ⚠ [480–534] | 1.88 |
| spsc_share | 5 | 121 [120–124] | 8.26 |
| spsc_xsp | 5 | 87.43 [84.93–88.13] | 11.44 |
| mpmc_share | 5 | 37.14 [36.18–37.45] | 26.93 |
| mpmc_cell | 5 | 33.99 [33.64–36.09] | 29.42 |
| mpmc_xsp | 5 | 32.95 [32.67–33.31] | 30.35 |
| mc_cell | 5 | 30.49 ⚠ [27.57–30.55] | 32.80 |
| mc_xsp | 5 | 25.93 [25.83–26.09] | 38.57 |

## W10  E8  wait=spin  (smt)  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 529 ⚠ [479–531] | 1.89 |
| spsc_share | 5 | 111 [110–111] | 9.04 |
| spsc_xsp | 5 | 37.90 [37.78–37.99] | 26.38 |
| mpmc_share | 5 | 37.21 [37.02–38.55] | 26.88 |
| mpmc_cell | 5 | 35.62 [33.81–35.75] | 28.07 |
| mpmc_xsp | 5 | 33.99 [32.16–34.85] | 29.42 |
| mc_cell | 5 | 30.30 ⚠ [27.30–30.42] | 33.00 |
| mc_xsp | 5 | 19.10 [18.89–19.22] | 52.34 |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
