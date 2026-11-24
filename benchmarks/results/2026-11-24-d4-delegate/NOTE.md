# d4-delegate: LevelDB's DB mutex delegated, 24 November 2026

The first timed leg of the delegation of critical sections (`wtc-cc
--delegate`, PR #68, measured at bf2be4a). `harness/leg_d4.py` on focs, a
bench reservation of 24 cores with their SMT siblings offline (processors 4
to 27), governor performance, turbo off, 1.90 GHz on every run, 10
repetitions, arms rotated by repetition, every run a fresh process under the
host's exclusive measurement lock. It ran 12:36 to 13:25 local (04:36 to
05:25 UTC), announced through the host's leg driver; its END line reads
"busy mean 0.087; above 0.10 on 4-27", nothing outside the reservation.
The per-run gate (mean busy share of the processors outside 4-27, limit
0.10) retired none of the 720 runs (median 0.0035, highest 0.033).
Controls (the original at 8 threads, three before and three after): 0.892
and 0.895 M reads/s, +0.3%; across repetitions no drift (last third 1.006
of the first). The A/A arm (the original's binary under a second name) is
0.99 to 1.01 of the original at every thread count.

## What ran

`db_bench readrandom` over one LevelDB database (1 M keys of `fillrandom`,
100-byte values, tables mmap'd), copied fresh to `/dev/shm` before every
run so that no run sees another's compactions; 3 M reads a run, divided
among T client threads. For T threads every arm gets the same processors,
4 to 3+T plus 27; the servers of the delegated builds are pinned to 27.

| arm | build |
|---|---|
| orig | LevelDB 1.23 (7ee830d) as released, clang++-18 -O2, `std::mutex` |
| wtc | the same program, library and `db_bench`, every file through ClangIR (`wtc-cc -O2 -fno-exceptions -fno-rtti`), nothing delegated |
| deleg | the same with `--delegate leveldb::DBImpl::mutex_ --delegate-assume-uniform-threads`: `Get`'s two sections and `GetSnapshot`'s run on the server, 33 sections refused; the server parks when idle (the default) |
| deleg-float | deleg with its server left unpinned |
| hand | probe Q4's hand-written delegation of `Get`'s two sections, clang++-18 |
| litl-mcs, litl-clh, litl-mcsstp | orig with a LiTL (916469c) lock preloaded: MCS spin, CLH spin, MCS spin-then-park; LiTL's interposition map exports `pthread_create` and `pthread_mutex_trylock` at GLIBC_2.34, without which it misses both on glibc 2.39 |

Every delegated run served 6.0 M sections, two per `Get`.
`harness/ldb-build.sh` builds wtc and deleg; `harness/summarize.py` makes
`SUMMARY.md` and `summary.csv` from `runs.jsonl`.

## Throughput

Million reads a second, all threads, median of 10:

| threads | orig | wtc | deleg | hand | LiTL MCS | LiTL CLH | LiTL MCS-STP |
|---|---|---|---|---|---|---|---|
| 1 | 0.509 | 0.569 | 0.258 | 0.259 | 0.470 | 0.476 | 0.472 |
| 2 | 0.748 | 0.837 | 0.445 | 0.440 | 0.700 | 0.684 | 0.701 |
| 4 | 0.953 | 0.983 | 0.777 | 0.745 | 0.982 | 0.991 | 0.967 |
| 8 | 0.903 | 0.901 | 1.293 | 1.126 | 1.344 | 1.215 | 1.289 |
| 12 | 0.868 | 0.924 | 1.435 | 1.292 | 1.406 | 1.220 | 1.338 |
| 16 | 0.849 | 0.877 | 1.521 | 1.348 | 1.395 | 1.266 | 1.331 |
| 20 | 0.855 | 0.902 | 1.508 | 1.377 | 1.377 | 1.224 | 1.324 |
| 23 | 0.850 | 0.908 | 1.515 | 1.361 | 1.376 | 1.288 | 0.140 |

| threads | deleg / orig | deleg / wtc | deleg / hand | deleg / best LiTL |
|---|---|---|---|---|
| 1 | 0.51 | 0.45 | 0.99 | 0.54 |
| 4 | 0.82 | 0.79 | 1.04 | 0.78 |
| 8 | 1.43 | 1.43 | 1.15 | 0.96 (MCS) |
| 12 | 1.65 | 1.55 | 1.11 | 1.02 (MCS) |
| 16 | 1.79 | 1.73 | 1.13 | 1.09 (MCS) |
| 23 | 1.78 | 1.67 | 1.11 | 1.10 (MCS) |

The original stops scaling at 4 threads and loses ground after it: the DB
mutex is handed between cores twice a read, and from 8 threads 44 to 81% of
its CPU time is in the kernel (futex waits, 0.9 to 1.6 M context switches a
run). The delegated build keeps the mutex's lines on the server and scales
to 16 threads, where the server saturates: 1.52, 1.51 and 1.52 M reads/s at
16, 20 and 23 threads, about 3.0 M sections a second, some 330 ns each. The
unpinned server costs nothing measurable (deleg-float 0.97 to 1.02 of
deleg). The compiler's build is 1.10 to 1.15 times the hand-written
delegation from 8 threads on and level with it below.

Below 8 threads delegation is a loss: a round trip to the server per
section costs more than an uncontended or lightly contended mutex, half the
throughput at one or two threads and 0.82 at four.

Against a better lock the gain is small. MCS, the best LiTL lock from 8
threads on, removes the kernel waits too and is ahead of delegation at 8
threads (0.96); delegation leads by 2 to 10% from 12 threads on. Probe
Q4's untimed figure of 1.45 times the best LiTL lock at 8 threads, on
another processor (a-p13, Xeon Gold 6230, turbo on), is not reproduced
here. The ClangIR build without delegation is 12% ahead of the original
at one and two threads and 0 to 7% from 8 threads on; deleg/wtc isolates
the delegation from that.

LiTL's MCS spin-then-park collapses at 23 threads (0.14 M reads/s, 5.9 M
context switches), where 23 clients and LevelDB's background thread fill
the 24 processors.

## CPU time

Medians; cores = (user + system) / wall time of the process, CPU seconds
per million reads:

| threads | orig | deleg | hand | LiTL MCS |
|---|---|---|---|---|
| 1 | 1.1 cores, 2.17 s | 2.0, 7.74 s | 2.0, 7.68 s | 1.1, 2.34 s |
| 4 | 4.0, 4.29 s | 4.9, 6.47 s | 4.9, 6.73 s | 4.1, 4.29 s |
| 8 | 7.5, 8.44 s | 8.7, 6.94 s | 8.5, 7.94 s | 8.1, 6.20 s |
| 16 | 15.1, 17.90 s | 16.4, 11.05 s | 16.3, 12.49 s | 15.8, 11.71 s |
| 23 | 21.9, 26.12 s | 23.3, 15.69 s | 23.0, 17.53 s | 22.6, 16.94 s |

The server is a core more than the original (+0.9 to +1.4 cores). It is
busy whenever a client is: it spins between sections and parks only when
idle. Per CPU second, delegation reads 0.28 of the original at one thread,
0.66 at four, 1.22 at eight and 1.62 to 1.66 from sixteen on; against MCS
0.30, 0.66, 0.89 and 1.06 to 1.08.

## What this says

On this host delegation pays where the mutex is contended (8 threads and
more) and costs where it is not (4 and fewer), which a compiler that only
knows the source cannot tell apart: the thread count is a run-time fact.
The compiler's delegation does at least as well as the hand-written one.
Against the original it is 1.43 to 1.79 times; against the best library
lock it is 0.96 to 1.10 times, which is below the 1.3 times probe Q4 set
for a decision.
