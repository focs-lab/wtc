# eval-ferret  (2026-10-21T10:21:14Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-21, smt on, build `compiler=tools/wtc-cc, ClangIR 1ebf89d8c5e6;wtc=915a601a933b;parsec=2e492f771c67;libraries=clang-18 -O2 -std=gnu89 -DHUGE=HUGE_VAL -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types; gsl by gcc;`, git 915a601a933b

## ferret  threads=1

| variant | n | time (s) | time (s) vs orig |
|---|---|---|---|
| hand-one | 10 | 3.45 [3.45–3.45] | 1.00× |
| orig | 10 | 3.45 [3.45–3.45] | 1.00× |
| generic-one | 10 | 3.47 [3.46–3.47] | 1.00× |
| compiler-one | 10 | 3.47 [3.47–3.47] | 1.00× |

## ferret  threads=2

| variant | n | time (s) | time (s) vs orig |
|---|---|---|---|
| orig | 10 | 1.74 [1.74–1.74] | 1.00× |
| compiler | 10 | 1.74 [1.74–1.75] | 1.00× |
| generic | 10 | 1.75 [1.75–1.76] | 1.01× |

## ferret  threads=4

| variant | n | time (s) | time (s) vs orig |
|---|---|---|---|
| orig | 10 | 0.89 [0.89–0.89] | 1.00× |
| compiler | 10 | 0.89 [0.89–0.89] | 1.00× |
| generic | 10 | 0.89 [0.89–0.89] | 1.01× |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
