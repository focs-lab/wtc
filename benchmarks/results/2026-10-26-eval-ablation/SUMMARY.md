# eval-ablation  (2026-10-26T05:37:41Z)

AMD EPYC 9115 16-Core Processor, kernel 6.8.0-137-generic, allowed cpus 16-31,48-63, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;tbb=0;`, git 

Control (shim_generic E64, median of 3 and 3): open 15.87, close 15.77 Mops/s, drift 0.6% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.001, rep 2 1.001, rep 3 0.998, rep 4 0.998 (n=240) — no trend.  Widest single row: spsc_cell in W10  E64  wait=spin  (smt)  alloc=glibc, 51% across repetitions.

## W10  E256  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 97.70 [95.36–98.25] | 10.24 |
| spsc_cell | 5 | 87.62 [86.61–90.71] | 11.41 |
| mc_cell | 5 | 28.37 [28.33–28.53] | 35.25 |
| mpmc_share | 5 | 16.65 [16.51–16.82] | 60.08 |
| mpmc_cell | 5 | 13.58 [13.35–14.03] | 73.64 |
| spsc_xsp | 5 | 9.08 [8.61–9.21] | 110 |
| mc_xsp | 5 | 6.73 [6.64–6.78] | 149 |
| mpmc_xsp | 5 | 6.22 [6.09–6.24] | 161 |

## W10  E256  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 109 ⚠ [95.09–109] | 9.19 |
| spsc_cell | 5 | 89.64 [86.90–89.96] | 11.16 |
| spsc_xsp | 5 | 28.26 [27.73–28.39] | 35.39 |
| mc_cell | 5 | 27.71 [27.36–27.91] | 36.09 |
| mpmc_xsp | 5 | 16.42 [16.36–16.77] | 60.91 |
| mc_xsp | 5 | 15.74 [15.52–15.78] | 63.53 |
| mpmc_share | 5 | 15.11 [14.46–15.25] | 66.19 |
| mpmc_cell | 5 | 13.94 [13.64–14.21] | 71.74 |

## W10  E256  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 105 [99.74–107] | 9.54 |
| spsc_cell | 5 | 89.84 ⚠ [87.94–108] | 11.13 |
| spsc_xsp | 5 | 33.23 [32.67–33.68] | 30.09 |
| mc_cell | 5 | 25.32 ⚠ [25.08–28.90] | 39.49 |
| mc_xsp | 5 | 18.76 ⚠ [17.03–18.94] | 53.30 |
| mpmc_share | 5 | 14.41 ⚠ [13.47–15.89] | 69.41 |
| mpmc_cell | 5 | 14.03 [13.46–14.17] | 71.28 |
| mpmc_xsp | 5 | 8.45 [8.33–8.65] | 118 |

## W10  E256  wait=spin  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 102 [97.84–104] | 9.85 |
| spsc_cell | 5 | 89.84 [89.01–90.39] | 11.13 |
| spsc_xsp | 5 | 89.53 [86.26–90.40] | 11.17 |
| mc_cell | 5 | 27.82 [27.77–27.87] | 35.94 |
| mc_xsp | 5 | 24.79 [24.33–25.12] | 40.33 |
| mpmc_xsp | 5 | 17.14 ⚠ [15.59–17.92] | 58.33 |
| mpmc_share | 5 | 14.12 [13.80–14.47] | 70.85 |
| mpmc_cell | 5 | 14.04 [13.94–14.28] | 71.20 |

## W10  E256  wait=spin  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_share | 5 | 98.28 ⚠ [87.08–115] | 10.18 |
| spsc_cell | 5 | 89.43 [88.08–90.04] | 11.18 |
| mc_cell | 5 | 27.83 [26.62–28.04] | 35.93 |
| spsc_xsp | 5 | 19.57 ⚠ [17.65–21.00] | 51.09 |
| mpmc_share | 5 | 14.14 [13.48–14.23] | 70.72 |
| mpmc_cell | 5 | 14.10 [13.56–14.22] | 70.92 |
| mpmc_xsp | 5 | 14.04 [13.90–14.12] | 71.20 |
| mc_xsp | 5 | 12.20 [12.10–12.32] | 81.94 |

## W10  E256  wait=spin  (smt)  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 252 [240–253] | 3.97 |
| spsc_share | 5 | 57.09 [55.67–57.62] | 17.52 |
| mc_cell | 5 | 28.01 [27.99–28.16] | 35.70 |
| mpmc_share | 5 | 26.62 [25.89–27.35] | 37.57 |
| mpmc_cell | 5 | 26.11 [25.15–26.55] | 38.30 |
| spsc_xsp | 5 | 9.52 [9.43–9.80] | 105 |
| mc_xsp | 5 | 8.53 [8.34–8.56] | 117 |
| mpmc_xsp | 5 | 7.30 [7.23–7.43] | 137 |

## W10  E256  wait=spin  (smt)  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 252 ⚠ [233–253] | 3.97 |
| spsc_share | 5 | 74.44 [72.55–76.11] | 13.43 |
| mpmc_share | 5 | 37.43 ⚠ [36.05–40.04] | 26.71 |
| mpmc_cell | 5 | 31.10 [31.06–32.95] | 32.15 |
| mpmc_xsp | 5 | 27.12 [26.79–27.50] | 36.87 |
| mc_cell | 5 | 26.61 [26.48–26.82] | 37.58 |
| spsc_xsp | 5 | 20.74 [20.69–21.22] | 48.21 |
| mc_xsp | 5 | 14.75 ⚠ [13.30–14.84] | 67.78 |

## W10  E256  wait=spin  (smt)  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 250 [249–251] | 4.01 |
| spsc_share | 5 | 58.35 [56.58–58.42] | 17.14 |
| mpmc_share | 5 | 39.80 [39.41–40.15] | 25.12 |
| spsc_xsp | 5 | 39.08 [37.94–39.34] | 25.59 |
| mpmc_cell | 5 | 31.80 ⚠ [29.45–32.09] | 31.45 |
| mc_cell | 5 | 27.34 [26.89–28.37] | 36.57 |
| mc_xsp | 5 | 19.67 [19.48–19.76] | 50.85 |
| mpmc_xsp | 5 | 19.57 ⚠ [18.25–20.48] | 51.09 |

## W10  E256  wait=spin  (smt)  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 251 [248–251] | 3.99 |
| spsc_share | 5 | 82.54 [82.28–82.61] | 12.12 |
| spsc_xsp | 5 | 55.94 [54.06–56.34] | 17.88 |
| mpmc_xsp | 5 | 37.24 ⚠ [33.77–37.34] | 26.85 |
| mpmc_share | 5 | 34.52 [33.30–35.27] | 28.96 |
| mpmc_cell | 5 | 30.57 [30.11–31.88] | 32.72 |
| mc_cell | 5 | 26.06 [24.33–26.10] | 38.37 |
| mc_xsp | 5 | 22.91 [22.30–23.04] | 43.64 |

## W10  E256  wait=spin  (smt)  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 248 [232–250] | 4.03 |
| spsc_share | 5 | 84.46 [82.36–85.49] | 11.84 |
| mpmc_share | 5 | 33.80 [33.50–34.91] | 29.58 |
| mpmc_xsp | 5 | 32.21 ⚠ [29.55–32.90] | 31.04 |
| mpmc_cell | 5 | 31.70 [31.21–32.24] | 31.55 |
| spsc_xsp | 5 | 28.73 [28.49–28.87] | 34.80 |
| mc_cell | 5 | 27.46 [27.19–27.55] | 36.41 |
| mc_xsp | 5 | 17.15 [17.09–17.17] | 58.32 |

## W10  E64  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 271 [266–277] | 3.69 |
| spsc_share | 5 | 112 [109–112] | 8.96 |
| mc_cell | 5 | 34.07 [33.51–34.42] | 29.35 |
| spsc_xsp | 5 | 16.89 [16.67–17.31] | 59.20 |
| mpmc_share | 5 | 16.57 [16.45–16.86] | 60.34 |
| mpmc_cell | 5 | 16.05 [15.99–16.20] | 62.30 |
| mc_xsp | 5 | 11.61 [11.56–11.84] | 86.15 |
| mpmc_xsp | 5 | 7.55 [7.37–7.64] | 132 |

## W10  E64  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 277 [267–280] | 3.61 |
| spsc_share | 5 | 149 [147–150] | 6.69 |
| spsc_xsp | 5 | 31.47 [30.95–31.57] | 31.77 |
| mc_cell | 5 | 30.57 [29.43–31.80] | 32.71 |
| mpmc_xsp | 5 | 18.49 [17.67–18.86] | 54.08 |
| mc_xsp | 5 | 16.94 [16.85–17.00] | 59.02 |
| mpmc_share | 5 | 16.16 [16.03–16.61] | 61.87 |
| mpmc_cell | 5 | 15.97 [15.94–16.05] | 62.61 |

## W10  E64  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 276 [274–278] | 3.62 |
| spsc_share | 5 | 124 [124–125] | 8.04 |
| spsc_xsp | 5 | 63.73 ⚠ [54.20–64.14] | 15.69 |
| mc_cell | 5 | 35.68 [34.84–35.95] | 28.03 |
| mc_xsp | 5 | 19.52 ⚠ [19.48–22.08] | 51.22 |
| mpmc_share | 5 | 16.12 ⚠ [15.82–17.42] | 62.02 |
| mpmc_cell | 5 | 15.82 [15.79–15.89] | 63.22 |
| mpmc_xsp | 5 | 8.19 ⚠ [8.10–9.38] | 122 |

## W10  E64  wait=spin  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 279 [274–282] | 3.59 |
| spsc_share | 5 | 158 [158–159] | 6.32 |
| spsc_xsp | 5 | 133 [132–134] | 7.53 |
| mc_cell | 5 | 35.30 [35.14–35.35] | 28.33 |
| mc_xsp | 5 | 26.70 [26.52–27.02] | 37.45 |
| mpmc_cell | 5 | 15.99 [15.86–16.05] | 62.55 |
| mpmc_share | 5 | 15.87 [15.67–16.15] | 63.00 |
| mpmc_xsp | 5 | 15.13 [14.92–15.62] | 66.07 |

## W10  E64  wait=spin  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 271 ⚠ [252–278] | 3.69 |
| spsc_share | 5 | 157 ⚠ [93.98–158] | 6.36 |
| mc_cell | 5 | 30.74 ⚠ [28.12–31.08] | 32.53 |
| spsc_xsp | 5 | 26.87 ⚠ [19.78–27.07] | 37.22 |
| mpmc_cell | 5 | 15.76 ⚠ [14.09–16.09] | 63.47 |
| mpmc_share | 5 | 15.50 ⚠ [13.70–15.64] | 64.50 |
| mc_xsp | 5 | 15.30 [15.18–15.41] | 65.34 |
| mpmc_xsp | 5 | 14.01 [13.75–14.41] | 71.39 |

## W10  E64  wait=spin  (smt)  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 435 ⚠ [290–436] | 2.30 |
| spsc_share | 5 | 61.94 [61.71–62.28] | 16.15 |
| mpmc_share | 5 | 38.80 [36.69–39.06] | 25.77 |
| mpmc_cell | 5 | 33.06 [32.20–33.78] | 30.25 |
| mc_cell | 5 | 30.60 [30.44–30.87] | 32.68 |
| spsc_xsp | 5 | 21.89 [21.66–21.95] | 45.69 |
| mc_xsp | 5 | 15.60 [15.53–15.62] | 64.11 |
| mpmc_xsp | 5 | 14.66 [14.44–14.74] | 68.21 |

## W10  E64  wait=spin  (smt)  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 435 [426–436] | 2.30 |
| spsc_share | 5 | 92.41 [91.81–92.65] | 10.82 |
| mpmc_share | 5 | 39.87 [39.62–41.59] | 25.08 |
| mpmc_cell | 5 | 35.05 ⚠ [30.22–36.10] | 28.53 |
| mpmc_xsp | 5 | 33.08 [32.27–33.60] | 30.23 |
| mc_cell | 5 | 28.53 [27.73–28.71] | 35.05 |
| spsc_xsp | 5 | 23.71 [23.30–24.05] | 42.17 |
| mc_xsp | 5 | 15.58 [15.55–15.67] | 64.18 |

## W10  E64  wait=spin  (smt)  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 436 [426–437] | 2.29 |
| spsc_share | 5 | 68.51 [67.92–68.65] | 14.60 |
| spsc_xsp | 5 | 48.56 [48.40–49.55] | 20.59 |
| mpmc_share | 5 | 38.58 ⚠ [38.09–44.12] | 25.92 |
| mpmc_cell | 5 | 34.28 [33.67–34.89] | 29.17 |
| mc_cell | 5 | 28.97 ⚠ [28.88–31.08] | 34.52 |
| mpmc_xsp | 5 | 24.56 ⚠ [22.91–25.94] | 40.72 |
| mc_xsp | 5 | 21.36 [21.22–21.96] | 46.81 |

## W10  E64  wait=spin  (smt)  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 436 [435–436] | 2.30 |
| spsc_share | 5 | 108 [107–108] | 9.26 |
| spsc_xsp | 5 | 75.56 [74.84–75.97] | 13.23 |
| mpmc_share | 5 | 36.42 [35.42–37.45] | 27.46 |
| mpmc_cell | 5 | 35.38 [33.43–35.83] | 28.27 |
| mpmc_xsp | 5 | 33.42 [32.84–34.88] | 29.92 |
| mc_cell | 5 | 28.57 [28.48–28.66] | 35.01 |
| mc_xsp | 5 | 24.42 [24.41–24.82] | 40.95 |

## W10  E64  wait=spin  (smt)  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 436 [434–436] | 2.30 |
| spsc_share | 5 | 102 [101–102] | 9.85 |
| mpmc_share | 5 | 37.96 ⚠ [33.95–39.33] | 26.35 |
| spsc_xsp | 5 | 36.31 ⚠ [31.61–36.41] | 27.54 |
| mpmc_cell | 5 | 35.52 ⚠ [32.10–36.28] | 28.15 |
| mpmc_xsp | 5 | 33.19 ⚠ [29.04–34.04] | 30.13 |
| mc_cell | 5 | 28.96 [26.88–29.06] | 34.53 |
| mc_xsp | 5 | 18.67 [18.54–18.74] | 53.56 |

## W10  E8  wait=spin  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 888 ⚠ [794–896] | 1.13 |
| spsc_share | 5 | 121 [117–121] | 8.27 |
| mc_cell | 5 | 36.96 [36.15–37.18] | 27.06 |
| mpmc_share | 5 | 31.01 ⚠ [23.11–39.91] | 32.24 |
| spsc_xsp | 5 | 17.85 [17.67–18.09] | 56.03 |
| mpmc_cell | 5 | 16.35 [16.10–16.40] | 61.18 |
| mc_xsp | 5 | 11.97 [11.93–12.08] | 83.53 |
| mpmc_xsp | 5 | 7.34 [7.29–7.46] | 136 |

## W10  E8  wait=spin  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 884 ⚠ [629–895] | 1.13 |
| spsc_share | 5 | 194 [194–194] | 5.15 |
| spsc_xsp | 5 | 31.11 [31.08–31.30] | 32.14 |
| mc_cell | 5 | 31.03 [30.80–31.08] | 32.23 |
| mpmc_xsp | 5 | 17.34 [17.06–17.98] | 57.65 |
| mpmc_share | 5 | 16.75 [16.51–17.06] | 59.70 |
| mc_xsp | 5 | 16.64 [16.41–16.89] | 60.09 |
| mpmc_cell | 5 | 16.47 [16.34–16.86] | 60.72 |

## W10  E8  wait=spin  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 883 ⚠ [771–889] | 1.13 |
| spsc_share | 5 | 159 [152–160] | 6.29 |
| spsc_xsp | 5 | 73.55 [69.47–73.83] | 13.60 |
| mc_cell | 5 | 36.84 ⚠ [33.79–38.42] | 27.15 |
| mc_xsp | 5 | 20.72 ⚠ [20.58–24.06] | 48.27 |
| mpmc_cell | 5 | 16.35 [16.22–16.62] | 61.15 |
| mpmc_share | 5 | 16.16 [16.11–16.36] | 61.86 |
| mpmc_xsp | 5 | 8.45 [8.31–8.53] | 118 |

## W10  E8  wait=spin  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 887 ⚠ [782–904] | 1.13 |
| spsc_share | 5 | 238 ⚠ [214–239] | 4.20 |
| spsc_xsp | 5 | 151 [149–157] | 6.62 |
| mc_cell | 5 | 36.97 [36.74–37.36] | 27.05 |
| mc_xsp | 5 | 29.50 [28.47–29.60] | 33.90 |
| mpmc_share | 5 | 16.80 [16.48–16.85] | 59.53 |
| mpmc_cell | 5 | 16.49 [16.40–16.86] | 60.64 |
| mpmc_xsp | 5 | 13.73 [13.53–13.79] | 72.85 |

## W10  E8  wait=spin  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 881 ⚠ [771–895] | 1.13 |
| spsc_share | 5 | 215 [215–226] | 4.64 |
| mc_cell | 5 | 31.09 [31.02–31.25] | 32.16 |
| spsc_xsp | 5 | 30.86 [28.96–31.15] | 32.40 |
| mpmc_share | 5 | 16.59 [16.10–16.73] | 60.29 |
| mpmc_cell | 5 | 16.56 [15.93–16.71] | 60.39 |
| mc_xsp | 5 | 16.34 [15.64–16.70] | 61.22 |
| mpmc_xsp | 5 | 14.27 [14.22–14.36] | 70.09 |

## W10  E8  wait=spin  (smt)  alloc=glibc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 531 [530–537] | 1.88 |
| spsc_share | 5 | 68.81 [68.29–71.87] | 14.53 |
| mpmc_share | 5 | 42.74 [42.39–42.84] | 23.40 |
| mpmc_cell | 5 | 33.96 [33.43–35.52] | 29.45 |
| mc_cell | 5 | 31.66 [30.92–31.93] | 31.58 |
| spsc_xsp | 5 | 22.85 [22.61–23.66] | 43.77 |
| mc_xsp | 5 | 15.91 [15.79–15.93] | 62.86 |
| mpmc_xsp | 5 | 14.01 [13.96–14.40] | 71.35 |

## W10  E8  wait=spin  (smt)  alloc=jemalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 534 [521–537] | 1.87 |
| spsc_share | 5 | 114 [114–114] | 8.78 |
| mpmc_share | 5 | 38.37 [38.02–38.70] | 26.06 |
| mpmc_cell | 5 | 35.12 [34.04–36.20] | 28.47 |
| mpmc_xsp | 5 | 34.42 [33.80–34.59] | 29.05 |
| mc_cell | 5 | 29.07 [28.90–29.46] | 34.40 |
| spsc_xsp | 5 | 23.64 [23.58–23.82] | 42.31 |
| mc_xsp | 5 | 15.56 [15.50–15.61] | 64.26 |

## W10  E8  wait=spin  (smt)  alloc=mimalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 534 ⚠ [424–537] | 1.87 |
| spsc_share | 5 | 86.13 [85.87–86.40] | 11.61 |
| spsc_xsp | 5 | 54.96 ⚠ [51.87–57.85] | 18.20 |
| mpmc_share | 5 | 36.58 [35.47–37.08] | 27.34 |
| mpmc_cell | 5 | 33.44 [33.36–35.38] | 29.90 |
| mpmc_xsp | 5 | 33.07 [31.15–33.55] | 30.24 |
| mc_cell | 5 | 29.67 ⚠ [29.14–32.15] | 33.70 |
| mc_xsp | 5 | 22.13 [21.94–23.06] | 45.19 |

## W10  E8  wait=spin  (smt)  alloc=snmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 536 [534–537] | 1.87 |
| spsc_share | 5 | 139 [134–140] | 7.21 |
| spsc_xsp | 5 | 87.99 [86.14–88.21] | 11.36 |
| mpmc_share | 5 | 36.93 [36.38–37.51] | 27.08 |
| mpmc_xsp | 5 | 34.08 [33.67–34.45] | 29.34 |
| mpmc_cell | 5 | 33.45 [33.15–35.30] | 29.89 |
| mc_cell | 5 | 29.49 [29.11–29.71] | 33.91 |
| mc_xsp | 5 | 26.02 [25.00–26.32] | 38.44 |

## W10  E8  wait=spin  (smt)  alloc=tcmalloc

| variant | n | Mops/s | ns/op |
|---|---|---|---|
| spsc_cell | 5 | 534 [529–535] | 1.87 |
| spsc_share | 5 | 130 [129–130] | 7.71 |
| spsc_xsp | 5 | 38.18 [37.90–38.47] | 26.19 |
| mpmc_share | 5 | 36.95 [35.52–37.76] | 27.07 |
| mpmc_cell | 5 | 33.93 [33.42–34.67] | 29.47 |
| mpmc_xsp | 5 | 33.39 [32.49–35.10] | 29.95 |
| mc_cell | 5 | 29.15 ⚠ [26.61–29.64] | 34.31 |
| mc_xsp | 5 | 19.21 [19.15–19.35] | 52.06 |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
