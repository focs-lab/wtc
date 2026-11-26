# progress-rt: the progress runtime's queues against main, 26 November 2026

The timed leg of the progress runtime (e7ba584; the series was
amended after it only in comments, documentation and one test, so the
runtime code of the commit before this one is the code measured) against main (83270b5) on the queue
microbenchmarks that go through the runtime's queues: W1, W9, W10, W11
and the bounded queue (WB), the 22 cells of the untimed A/B that
preceded the series' first timed leg
(`benchmarks/scripts/progress_rt_leg/cells.txt`), W11 cell_mpmc and
cell_spmc with one worker among them. Three arms: main, main-aa (a
second build of the same export of main, its binaries byte for byte
main's) and pr. Each arm was built by
`benchmarks/scripts/progress_rt_leg/build.sh` from a clean
`git archive` of its commit.

`leg_pr72.py` on focs, a bench reservation of 24 cores with their SMT
siblings offline (processors 4 to 27), governor performance, turbo off,
1.90 GHz on every run, 10 repetitions, arms rotated by repetition inside
each cell, every run a fresh process under the host's exclusive
measurement lock; two warm-up runs and three control runs before and
after. It ran 00:00 to 00:19 UTC, announced through the host's leg
driver (`chain_pr72.sh`), which waited for no compile on the host
before START. Its END line:

    END progress-rt rc=0; busy mean 0.034; above 0.10 on 4-8

Nothing outside the reservation was above 0.10 over the leg. The per-run
gate (mean busy share of the processors outside 4-27, limit 0.10) retired
none of the 660 runs (median 0.0035, highest 0.029). No run failed. From
00:03 to 00:14 single processors outside the set were busy for part of a
run (the busiest, over one run, at 1.000), in every arm alike and at a
mean of under 0.03 over the processors outside the set.

An earlier timed leg of the series, at its previous head (25 November),
found four cells lower than main: W1 shim_generic E64 0.85,
shim_generic_move E64 0.82, W9 split shim_mpsc 1:1 0.86 and W11
cell_spmc E64 with one worker 0.96. The series was changed after it, and
its numbers no longer describe the branch; this leg replaces it.

## Cells

Mops/s (W11: million tasks a second), median of 10. pr/main is the ratio
of medians with a bootstrap 95% interval; a cell is marked moved when
pr/main is more than 3% from 1, its interval excludes 1 and it is
further from 1 than the cell's A/A. `SUMMARY.md` has every arm's
interval, range and CV.

| cell | main | pr | pr/main [95% CI] | A/A (main-aa/main) |
|---|---|---|---|---|
| W1 shim_generic E64 | 4.557 | 4.516 | 0.991 [0.840-1.253] | 1.005 |
| W1 shim_generic_move E64 | 4.327 | 4.411 | 1.019 [0.993-1.042] | 1.016 |
| W1 shim_spsc E64 | 32.442 | 34.931 | 1.077 [1.057-1.109] **moved** | 1.004 |
| W10 mpmc_cell E64 | 4.588 | 4.429 | 0.965 [0.941-1.009] | 0.989 |
| W10 spsc_cell E64 | 126.078 | 124.346 | 0.986 [0.962-1.006] | 1.008 |
| W9 split shim_generic 2:2 | 6.502 | 6.551 | 1.008 [0.981-1.032] | 0.999 |
| W9 split shim_generic 4:4 | 8.620 | 8.568 | 0.994 [0.981-1.010] | 0.997 |
| W9 split shim_generic 1:2 | 2.957 | 2.991 | 1.012 [0.962-1.058] | 1.006 |
| W9 split shim_mpsc 1:1 | 15.134 | 14.939 | 0.987 [0.935-1.045] | 0.998 |
| W9 split shim_mpsc 2:1 | 7.999 | 8.169 | 1.021 [1.014-1.045] | 1.003 |
| W9 split shim_mpsc 4:1 | 12.523 | 12.467 | 0.996 [0.974-1.007] | 0.993 |
| W9 split shim_mpsc 8:1 | 13.359 | 13.389 | 1.002 [0.982-1.017] | 1.001 |
| W9 split orig_mpsc 2:1 | 3.139 | 3.215 | 1.024 [0.976-1.067] | 0.967 |
| W9 split shim_spmc 1:2 | 12.798 | 12.509 | 0.977 [0.960-1.020] | 0.985 |
| W11 cell_mpmc E64 w1 | 4.322 | 4.191 | 0.970 [0.935-0.987] **moved** | 1.000 |
| W11 cell_mpmc E64 w4 | 3.187 | 3.159 | 0.991 [0.978-0.999] | 1.001 |
| W11 cell_spmc E64 w1 | 38.977 | 37.996 | 0.975 [0.971-0.978] | 1.000 |
| WB split bounded_spsc 1:1 | 3.925 | 4.289 | 1.093 [0.896-1.317] | 1.006 |
| WB split bounded_mpsc 2:1 | 4.371 | 4.467 | 1.022 [0.770-1.252] | 0.964 |
| WB split bounded_spmc 1:2 | 5.675 | 5.403 | 0.952 [0.747-1.073] | 1.007 |
| WB split bounded_spmc 1:4 | 4.445 | 4.155 | 0.935 [0.843-1.277] | 0.990 |
| WB split bounded_generic 1:1 | 4.220 | 4.072 | 0.965 [0.808-1.118] | 0.979 |

Of the earlier leg's four, three are level with main now: W1
shim_generic 0.991, shim_generic_move 1.019, W9 split shim_mpsc 1:1
0.987.

Two cells are marked:

- W1 shim_spsc E64 is faster, 1.077 [1.057-1.109]. pr's runs (34.11 to
  35.19) lie above every run of main and main-aa (30.39 to 32.79).
- W11 cell_mpmc E64 with one worker is slower, 0.970 [0.935-0.987],
  just at the threshold. From the second repetition on, pr's runs are
  3.97 to 4.27 and main's are 4.22 to 4.53, so the ranges overlap.

W11 cell_spmc E64 with one worker is not marked, because its ratio is
within 3% of 1. It is still a loss: 0.975 [0.971-0.978], A/A 1.000. From
the second repetition on, pr's runs (37.87 to 38.16) lie below every run
of main (38.86 to 39.12). That is less than the 0.957 of the earlier
leg, but the cell has not come back to main.

W9 split shim_mpsc 1:1 ran in two modes, in step in every arm:
repetitions 1 to 4 at 14.2 to 14.4, the rest at 14.9 to 15.3. Within
each repetition pr is 0.6 to 1.8% below main, which is inside the cell's
interval.

The bounded queue's cells (WB) have a CV of 9 to 22% in main and pr, and
their runs fall in two modes in every arm. Their pr/main of 0.94 to 1.09
has wide intervals (0.43 wide at bounded_spmc 1:4). They show no change
and could not show one of under about 20%.

## Controls

W1 shim_generic E64 on main, three runs before and three after: start
6.39, 6.11, 6.60 (median 6.391), end 4.43, 4.56, 4.29 (median 4.425),
-30.8%, beyond the 5% threshold. As in the earlier leg, the drop comes
from the opening minutes, not from the leg. That cell's first two
repetitions were also high in all three arms (6.10 to 7.00), against
4.30 to 5.13 from the third on, and the end controls match the cell's
median on main (4.557). The drift check over all configurations reads
1.000 for the first third of the repetitions and 1.010 for the last.

## Files

`runs.jsonl` (every run, the binaries' paths written as $PR72_BIN),
`log.txt` (the leg's log, with the environment check), `SUMMARY.md`
and `summary.csv` (`summarize_pr72.py`), `meta.json`. The harness is
in `benchmarks/scripts/progress_rt_leg/`.
