# d4-adapt: delegation only while the mutex is contended, 24 November 2026

The timed leg of the adaptive delegation of critical sections (the
runtime as it stood at fc9e6b9, an earlier state of this series' runtime
commit (below): a section runs on its client while its mutex is free
or cold, and is delegated while the mutex is hot, `docs/delegation.md`,
"Where a section runs"), against the delegation of every section measured
in `2026-11-24-d4-delegate` that morning. `harness/leg_d4a.py` on focs, a
bench reservation of 24 cores with their SMT siblings offline (processors
4 to 27), governor performance, turbo off, 1.90 GHz on every run, 10
repetitions, arms rotated by repetition, every run a fresh process under
the host's exclusive measurement lock. It ran 20:52 to 21:23 local (12:52
to 13:23 UTC), announced through the host's leg driver
(`harness/chain_d4a.sh`), which waited for no compile on the host before
START; its END line reads "busy mean 0.075; above 0.10 on 4-19,27",
nothing outside the reservation. The per-run gate (mean busy share of the
processors outside 4-27, limit 0.10) retired none of the 490 runs (median
0.0039, highest 0.055).

Two earlier legs of the same day are not used. One, at f084d5d, measured
the runtime before a review of it found that its average rounded small
times away and weighed only contended sections, that a cooled mutex kept
its server and a later hot one could get none, and three faults dating
from the first delegation runtime (errno, statics a fork could leave
locked, a gate table probed 16 deep); the runtime was changed and its
numbers no longer describe it. The other, of the same build, 20:19 to 20:51,
ran while processors outside the reservation were busy (END line "above
0.10 on 0-19,27,56-59", every run's gate at about 0.12) and was retired
by the END-line rule.

The runtime commit was changed after the leg (a7ca553: free counts kept
per mutex for a thread that goes between mutexes, a hot mutex cooling
after 10 ms with no section, errno kept while the environment is read,
the decay of free sections made exact). An untimed A/B of the leg's
binary against a7ca553's, in the same reservation, at 1, 4, 8, 16 and 23
threads with 5 to 13 repetitions, showed no change beyond noise:
throughput 0.99 to 1.00 of the leg's binary (medians; 0.96 at 23 threads
in the first 5 repetitions, 0.995 in 8 more), the delegated fraction the
same within 0.01 at every count. The data stand for a7ca553.

How far to trust it. The A/A arm (the original's binary under a second
name) is 0.96 to 1.03 of the original; the controls, the original at 8
threads three times before and after, moved -3.2%; the drift check
across repetitions reads 1.004 (last third against first). The original
and the ClangIR build without delegation have a CV over 3% at most
thread counts from 4 on, their threads 46 to 83% in the kernel from 8 on; a few per cent between them is
not a result.

## What ran

`db_bench readrandom` as in the morning leg: one LevelDB database (1 M
keys of `fillrandom`, 100-byte values), copied fresh to `/dev/shm` before
every run, 3 M reads a run divided among T client threads, T = 1, 2, 4,
8, 12, 16, 23. For T threads every arm gets processors 4 to 3+T plus 27;
the servers are pinned to 27.

| arm | build |
|---|---|
| orig, orig-aa | LevelDB 1.23 (7ee830d) as released, clang++-18 -O2, `std::mutex`: the morning's binary |
| wtc | every file through ClangIR, nothing delegated: the morning's binary |
| deleg | `--delegate leveldb::DBImpl::mutex_ --delegate-assume-uniform-threads` at bf2be4a, every section delegated: the morning's binary |
| deleg-always | the adapt binary run with `WTC_DELEGATE_MODE=always`, every section delegated |
| adapt | the same delegation built at fc9e6b9 (built before a runtime test of that commit was changed; rebuilt at fc9e6b9, byte for byte the same; see below for the runtime commit as it now is), the runtime's default: `Get`'s two sections and `GetSnapshot`'s delegated, 33 refused, as before |
| litl-mcs | orig with LiTL's MCS spinlock preloaded, the best LiTL lock of the morning leg from 8 threads on |

The harness is the morning's (`leg_d4.py`) with the arms deleg-always and
adapt added, the thread counts of the task (20 dropped), and the clients'
counts of where their sections ran read from the runtime's report. Before
the first of the day's adaptive legs, an untimed A/B ran both harnesses
twice on identical binaries in the same reservation: the new over the
old, geometric mean 1.022 over the 28 cells they share (0.86 to 1.14; the
original's and ClangIR's cells at 8 threads and more move by as much
between runs). `harness/summarize_d4a.py` makes `SUMMARY.md` and
`summary.csv`.

## Throughput

Million reads a second, all threads, median of 10:

| threads | orig | wtc | deleg (every section) | adapt | LiTL MCS |
|---|---|---|---|---|---|
| 1 | 0.518 | 0.577 | 0.249 | 0.550 | 0.473 |
| 2 | 0.749 | 0.839 | 0.437 | 0.811 | 0.709 |
| 4 | 0.970 | 0.993 | 0.783 | 0.989 | 0.994 |
| 8 | 0.858 | 0.898 | 1.302 | 1.318 | 1.348 |
| 12 | 0.871 | 0.907 | 1.516 | 1.555 | 1.402 |
| 16 | 0.876 | 0.934 | 1.569 | 1.664 | 1.391 |
| 23 | 0.870 | 0.867 | 1.664 | 1.737 | 1.379 |

| threads | adapt / orig | adapt / wtc | adapt / deleg | deleg-always / deleg | adapt / LiTL MCS |
|---|---|---|---|---|---|
| 1 | 1.06 | 0.95 | 2.21 | 1.01 | 1.16 |
| 2 | 1.08 | 0.97 | 1.86 | 1.03 | 1.14 |
| 4 | 1.02 | 1.00 | 1.26 | 1.01 | 0.99 |
| 8 | 1.54 | 1.47 | 1.01 | 1.02 | 0.98 |
| 12 | 1.79 | 1.71 | 1.03 | 1.04 | 1.11 |
| 16 | 1.90 | 1.78 | 1.06 | 1.05 | 1.20 |
| 23 | 2.00 | 2.00 | 1.04 | 1.07 | 1.26 |

The task's two goals hold: at one to four threads the adaptive build is
1.02 to 1.08 times the original (goal 0.97), and from eight threads on it
is 1.01 to 1.06 times the delegation of every section (goal: no loss). At
one to four threads it is 0.95 to 1.00 of the ClangIR build without
delegation, the cost of the gate (two atomic read-modify-writes a free
section, and the timing of the contended ones). deleg-always, the new
runtime told to delegate everything, is 1.01 to 1.07 times the morning's
runtime: from 12 threads on the new one is a little faster at the same
work, which is also part of adapt/deleg there.

Against the best LiTL lock MCS is level at 4 and 8 threads (0.98 to
0.99), and the delegation leads by 11 to 26% from 12 on. Where the
sections ran (the clients' own counts, summed over the repetitions):

| threads | free | contended, as written | delegated | runs with a server |
|---|---|---|---|---|
| 1 | 1.000 | 0 | 0 | 0 of 10 |
| 2 | 0.915 | 0.085 | 0 | 0 of 10 |
| 4 | 0.464 | 0.531 | 0.004 | 1 of 10 |
| 8 | 0.020 | 0.009 | 0.971 | 10 of 10 |
| 12 | 0.014 | 0.006 | 0.980 | 10 of 10 |
| 16 | 0.013 | 0.005 | 0.981 | 10 of 10 |
| 23 | 0.014 | 0.006 | 0.980 | 10 of 10 |

At two threads the mutex never turned hot; at four it did once in ten runs
(0.4% of the sections delegated); from eight on it is hot but for the
measurements after each epoch (262144 sections), which run 2 to 3% of
the sections on their clients.

## CPU time

Medians; cores = (user + system) / wall time of the process, CPU seconds
per million reads:

| threads | orig | deleg | adapt | LiTL MCS |
|---|---|---|---|---|
| 1 | 1.1 cores, 2.12 s | 2.0, 7.98 s | 1.1, 1.98 s | 1.1, 2.31 s |
| 2 | 2.1, 2.87 s | 3.0, 6.84 s | 2.1, 2.63 s | 2.1, 3.01 s |
| 4 | 4.0, 4.21 s | 4.9, 6.39 s | 4.0, 4.13 s | 4.1, 4.24 s |
| 8 | 7.4, 8.70 s | 8.7, 6.90 s | 8.6, 6.79 s | 8.0, 6.16 s |
| 12 | 11.2, 13.05 s | 12.5, 8.53 s | 12.4, 8.31 s | 11.9, 8.79 s |
| 16 | 15.1, 17.32 s | 16.4, 10.74 s | 16.2, 10.12 s | 15.8, 11.73 s |
| 23 | 21.9, 25.52 s | 23.2, 14.31 s | 22.9, 13.69 s | 22.6, 16.90 s |

Below eight threads the adaptive build costs no more cores than the
original, where the delegation of every section cost one more (the
server, busy whenever a client is): per CPU second it reads 1.02 to 1.09
times the original, against 0.27 to 0.66 for deleg. From eight threads
on it reads 1.02 to 1.06 times deleg per CPU second, 1.28 to 1.86 times
the original and 0.91 to 1.24 times MCS.

## What this says

The run-time choice removes the loss below eight threads without giving
up the gain above it, on this program and host: the build that names the
mutex is now at least the original at every thread count measured. The
threshold (750 ns of average wait a section) was chosen from untimed runs
on this host and this program (`docs/delegation.md`, "Where a section
runs"); how well it carries to another processor or another mutex is not
measured.
