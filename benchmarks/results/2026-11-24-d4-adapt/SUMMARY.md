# d4a 2026-11-24-d4-adapt

runs 490 measured, 0 retired (gate 0.1 mean busy outside the set, or rc != 0)
busy outside the set per run: median 0.0039, max 0.0553; max single processor 0.481
cpu4 scaling_cur_freq seen: 1899961, 1899980, 1900000, 1900019, 1900038

controls (orig, 8 threads, ops/s): start median 8.661e+05 [866082, 1021842, 853060], end median 8.386e+05 [818498, 958199, 838574]: -3.2%
drift: configurations normalised to their first repetition, median of the first third 1.000, of the last third 1.004

## Throughput, M reads/s total: median [95% CI of the median] (min-max), n; * = CV > 3%

| T | orig | orig-aa | wtc | deleg | deleg-always | adapt | litl-mcs |
|---|---|---|---|---|---|---|---|
| 1 | 0.518 [0.515-0.519] (0.514-0.526) n10 | 0.518 [0.514-0.521] (0.513-0.524) n10 | 0.577 [0.572-0.582] (0.568-0.583) n10 | 0.249 [0.240-0.254] (0.237-0.257) n10 | 0.251 [0.249-0.264] (0.246-0.274) n10* | 0.550 [0.548-0.553] (0.542-0.558) n10 | 0.473 [0.470-0.475] (0.464-0.477) n10 |
| 2 | 0.749 [0.745-0.755] (0.740-0.758) n10 | 0.758 [0.755-0.760] (0.747-0.766) n10 | 0.839 [0.834-0.846] (0.830-0.854) n10 | 0.437 [0.432-0.457] (0.427-0.460) n10 | 0.451 [0.439-0.461] (0.434-0.465) n10 | 0.811 [0.806-0.817] (0.801-0.826) n10 | 0.709 [0.703-0.714] (0.694-0.715) n10 |
| 4 | 0.970 [0.948-0.998] (0.939-1.025) n10* | 0.961 [0.951-1.002] (0.946-1.035) n10* | 0.993 [0.977-1.033] (0.961-1.053) n10 | 0.783 [0.774-0.799] (0.750-0.811) n10 | 0.792 [0.785-0.809] (0.781-0.816) n10 | 0.989 [0.967-1.018] (0.946-1.055) n10* | 0.994 [0.959-1.010] (0.878-1.018) n10* |
| 8 | 0.858 [0.842-0.936] (0.841-1.018) n10* | 0.875 [0.838-0.963] (0.837-1.041) n10* | 0.898 [0.875-0.986] (0.863-1.043) n10* | 1.302 [1.292-1.312] (1.258-1.355) n10 | 1.328 [1.302-1.340] (1.289-1.357) n10 | 1.318 [1.303-1.343] (1.292-1.350) n10 | 1.348 [1.326-1.372] (1.318-1.389) n10 |
| 12 | 0.871 [0.844-0.929] (0.833-0.960) n10* | 0.898 [0.820-0.943] (0.801-0.999) n10* | 0.907 [0.866-0.978] (0.831-1.009) n10* | 1.516 [1.475-1.555] (1.445-1.584) n10 | 1.581 [1.546-1.588] (1.537-1.595) n10 | 1.555 [1.518-1.594] (1.478-1.647) n10* | 1.402 [1.388-1.416] (1.359-1.432) n10 |
| 16 | 0.876 [0.847-0.909] (0.815-0.998) n10* | 0.873 [0.836-0.899] (0.824-0.919) n10* | 0.934 [0.895-0.963] (0.835-1.015) n10* | 1.569 [1.529-1.600] (1.462-1.637) n10* | 1.643 [1.624-1.677] (1.614-1.757) n10 | 1.664 [1.614-1.702] (1.594-1.740) n10 | 1.391 [1.380-1.407] (1.354-1.435) n10 |
| 23 | 0.870 [0.827-0.893] (0.797-0.972) n10* | 0.838 [0.814-0.880] (0.809-0.898) n10* | 0.867 [0.852-0.900] (0.822-0.977) n10* | 1.664 [1.600-1.699] (1.514-1.734) n10* | 1.776 [1.722-1.804] (1.650-1.832) n10 | 1.737 [1.722-1.765] (1.664-1.806) n10 | 1.379 [1.365-1.408] (1.346-1.427) n10 |

## Ratios of medians

| T | aa/orig | wtc/orig | deleg/orig | adapt/orig | adapt/wtc | adapt/deleg | deleg-always/deleg | adapt/litl-mcs | litl-mcs/orig |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 1.00 | 1.11 | 0.48 | 1.06 | 0.95 | 2.21 | 1.01 | 1.16 | 0.91 |
| 2 | 1.01 | 1.12 | 0.58 | 1.08 | 0.97 | 1.86 | 1.03 | 1.14 | 0.95 |
| 4 | 0.99 | 1.02 | 0.81 | 1.02 | 1.00 | 1.26 | 1.01 | 0.99 | 1.02 |
| 8 | 1.02 | 1.05 | 1.52 | 1.54 | 1.47 | 1.01 | 1.02 | 0.98 | 1.57 |
| 12 | 1.03 | 1.04 | 1.74 | 1.79 | 1.71 | 1.03 | 1.04 | 1.11 | 1.61 |
| 16 | 1.00 | 1.07 | 1.79 | 1.90 | 1.78 | 1.06 | 1.05 | 1.20 | 1.59 |
| 23 | 0.96 | 1.00 | 1.91 | 2.00 | 2.00 | 1.04 | 1.07 | 1.26 | 1.58 |

## CPU: cores busy = (user+sys)/wall of the process; CPU seconds per million reads; system share; context switches per run (medians)

| T | orig | orig-aa | wtc | deleg | deleg-always | adapt | litl-mcs |
|---|---|---|---|---|---|---|---|
| 1 | 1.1c 2.12s/M sys2% cs0k | 1.1c 2.12s/M sys2% cs0k | 1.1c 1.90s/M sys2% cs0k | 2.0c 7.98s/M sys0% cs6k | 2.0c 7.90s/M sys1% cs5k | 1.1c 1.98s/M sys2% cs0k | 1.1c 2.31s/M sys1% cs0k |
| 2 | 2.1c 2.87s/M sys4% cs4k | 2.1c 2.83s/M sys4% cs4k | 2.1c 2.55s/M sys4% cs3k | 3.0c 6.84s/M sys1% cs1k | 3.0c 6.62s/M sys1% cs1k | 2.1c 2.63s/M sys4% cs2k | 2.1c 3.01s/M sys1% cs0k |
| 4 | 4.0c 4.21s/M sys16% cs172k | 4.0c 4.23s/M sys17% cs198k | 4.0c 4.08s/M sys19% cs212k | 4.9c 6.39s/M sys1% cs4k | 4.9c 6.30s/M sys1% cs5k | 4.0c 4.13s/M sys14% cs124k | 4.1c 4.24s/M sys1% cs0k |
| 8 | 7.4c 8.70s/M sys47% cs1046k | 7.4c 8.59s/M sys46% cs980k | 7.4c 8.30s/M sys48% cs1047k | 8.7c 6.90s/M sys3% cs22k | 8.7c 6.76s/M sys3% cs24k | 8.6c 6.79s/M sys4% cs41k | 8.0c 6.16s/M sys1% cs0k |
| 12 | 11.2c 13.05s/M sys64% cs1287k | 11.1c 12.57s/M sys63% cs1392k | 11.1c 12.43s/M sys65% cs1282k | 12.5c 8.53s/M sys4% cs61k | 12.5c 8.16s/M sys5% cs66k | 12.4c 8.31s/M sys6% cs76k | 11.9c 8.79s/M sys0% cs0k |
| 16 | 15.1c 17.32s/M sys73% cs1392k | 15.1c 17.52s/M sys73% cs1412k | 15.1c 16.40s/M sys74% cs1221k | 16.4c 10.74s/M sys6% cs83k | 16.3c 10.26s/M sys6% cs89k | 16.2c 10.12s/M sys7% cs95k | 15.8c 11.73s/M sys0% cs0k |
| 23 | 21.9c 25.52s/M sys82% cs1362k | 21.9c 26.57s/M sys82% cs1477k | 21.8c 25.50s/M sys83% cs1426k | 23.2c 14.31s/M sys7% cs90k | 23.0c 13.39s/M sys8% cs107k | 22.9c 13.69s/M sys9% cs108k | 22.6c 16.90s/M sys0% cs1k |

reads per CPU second (M, medians): adapt / deleg / wtc / orig / litl-mcs
  T=1: 0.505 / 0.125 / 0.527 / 0.471 / 0.432  (adapt/orig 1.07, adapt/deleg 4.03)
  T=2: 0.380 / 0.146 / 0.392 / 0.349 / 0.332  (adapt/orig 1.09, adapt/deleg 2.60)
  T=4: 0.242 / 0.156 / 0.245 / 0.238 / 0.236  (adapt/orig 1.02, adapt/deleg 1.55)
  T=8: 0.147 / 0.145 / 0.121 / 0.115 / 0.162  (adapt/orig 1.28, adapt/deleg 1.02)
  T=12: 0.120 / 0.117 / 0.080 / 0.077 / 0.114  (adapt/orig 1.57, adapt/deleg 1.03)
  T=16: 0.099 / 0.093 / 0.061 / 0.058 / 0.085  (adapt/orig 1.71, adapt/deleg 1.06)
  T=23: 0.073 / 0.070 / 0.039 / 0.039 / 0.059  (adapt/orig 1.86, adapt/deleg 1.04)

## Where the clients' sections ran (sums over the repetitions): free / contended as written / delegated

  deleg-always T=1: 0.000 / 0.000 / 1.000; runs with a server 10 of 10
  deleg-always T=2: 0.000 / 0.000 / 1.000; runs with a server 10 of 10
  deleg-always T=4: 0.000 / 0.000 / 1.000; runs with a server 10 of 10
  deleg-always T=8: 0.000 / 0.000 / 1.000; runs with a server 10 of 10
  deleg-always T=12: 0.000 / 0.000 / 1.000; runs with a server 10 of 10
  deleg-always T=16: 0.000 / 0.000 / 1.000; runs with a server 10 of 10
  deleg-always T=23: 0.000 / 0.000 / 1.000; runs with a server 10 of 10
  adapt T=1: 1.000 / 0.000 / 0.000; runs with a server 0 of 10
  adapt T=2: 0.915 / 0.085 / 0.000; runs with a server 0 of 10
  adapt T=4: 0.464 / 0.531 / 0.004; runs with a server 1 of 10
  adapt T=8: 0.020 / 0.009 / 0.971; runs with a server 10 of 10
  adapt T=12: 0.014 / 0.006 / 0.980; runs with a server 10 of 10
  adapt T=16: 0.013 / 0.005 / 0.981; runs with a server 10 of 10
  adapt T=23: 0.014 / 0.006 / 0.980; runs with a server 10 of 10

served sections per deleg and deleg-always run: min 5999964 max 6000000
