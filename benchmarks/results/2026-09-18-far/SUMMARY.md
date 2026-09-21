# far  (2026-09-18T20:17:07Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4,5, smt on, build `compiler=Clang 18.1.3;build_type=Release;march=native;sanitize=;check=OFF;`, git 06689f0d99cb (dirty)

Control (shim_generic E64, median of 3 and 3): open 1.13, close 1.15 Mops/s, drift 1.4% — OK

Drift across repetitions, median over all configurations, relative to the first: rep 0 1.000, rep 1 1.008, rep 2 1.036 (n=4) — **drifting in one direction: treat ratios measured far apart with care**.  Widest single row: shim_spsc E64, 14% across repetitions.

## W1  E64  wait=spin

| variant | n | Mops/s | Mops/s vs shim_generic | vs ring_inline |
|---|---|---|---|---|
| ring_inline | 3 | 147 (146–148) | 130× | 1.00× |
| shim_spsc | 3 | 3.43 ⚠ (3.41–3.89) | 3.04× | 0.02× |
| shim_generic | 3 | 1.13 (1.12–1.15) | 1.00× | 1/130 |
| orig_generic | 3 | 1.11 ⚠ (1.07–1.16) | 0.98× | 1/132 |

Medians across repetitions; each repetition is a fresh process; CI is a bootstrap of the median. ⚠ marks CoV above 3%. Latencies in ns; tails are the median across runs of each run's own percentile, with the min–max spread of p99 in parentheses.
