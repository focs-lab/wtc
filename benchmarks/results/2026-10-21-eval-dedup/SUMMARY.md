# eval-dedup  (2026-10-21T10:28:36Z)

Intel(R) Xeon(R) w9-3495X, kernel 6.8.0-40-generic, allowed cpus 4-21, smt on, build `compiler=tools/wtc-cc, ClangIR 1ebf89d8c5e6;wtc=915a601a933b;parsec=2e492f771c67;others=clang-18 -O2 -fno-strict-aliasing -fcommon -pthread -D_XOPEN_SOURCE=600 -DENABLE_GZIP_COMPRESSION -DENABLE_PTHREADS -DENABLE_PARSEC_HOOKS -I<corpus>/parsec/pkgs/libs/hooks/src/include -DOPENSSL_NO_INLINE_ASM -I<corpus>/parsec/pkgs/kernels/dedup/src;`, git 915a601a933b

## dedup  threads=1

| variant | n | time (s) | time (s) vs orig |
|---|---|---|---|
| compiler-flat-one-unity | 10 | 20.97 [20.95–21.00] | 1.00× |
| orig | 10 | 20.97 [20.95–20.99] | 1.00× |
| hand-one | 10 | 21.05 [21.03–21.07] | 1.00× |
| hand-spsc-one | 10 | 21.09 [21.06–21.11] | 1.01× |
| compiler-one | 10 | 21.14 [21.12–21.16] | 1.01× |
| compiler-flat-one | 10 | 21.14 [21.12–21.18] | 1.01× |
| generic-one | 10 | 21.16 [21.15–21.18] | 1.01× |

## dedup  threads=2

| variant | n | time (s) | time (s) vs orig |
|---|---|---|---|
| orig | 10 | 10.86 [10.86–10.88] | 1.00× |
| generic | 10 | 11.02 [11.00–11.03] | 1.01× |
| compiler | 10 | 11.02 [11.02–11.03] | 1.01× |

## dedup  threads=4

| variant | n | time (s) | time (s) vs orig |
|---|---|---|---|
| orig | 10 | 5.65 [5.64–5.66] | 1.00× |
| generic | 10 | 5.76 [5.75–5.78] | 1.02× |
| compiler | 10 | 5.76 [5.75–5.77] | 1.02× |

Medians across repetitions; each repetition is a fresh process; in brackets the bootstrap 95% CI of the median of the column the table is sorted by. ⚠ marks CoV above 3%. ns/op is 1000 / Mops/s. Latencies in ns; tails are the median across runs of each run's own percentile. "sat" marks a row with a run that did not sustain its offered rate: its latency is a backlog.
