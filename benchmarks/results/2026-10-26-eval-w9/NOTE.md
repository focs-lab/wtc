# eval-w9: the general queue and its substitutes under many threads, 26 October 2026

`campaign.sh eval-w9` with `WTC_UNPINNED=1` on the second host, a
two-socket AMD EPYC 9115, socket 1's processors, 5 repetitions under
glibc, jemalloc and mimalloc, 8-byte elements: W9 in two modes.
`split` puts P producers and C consumers on their own threads (1:1, 1:2,
1:4, 1:8, 2:1, 4:1, 8:1, 2:2, 4:4, 8:8); `pairwise`, the workload of the
lock-free queue literature, has 1 to 16 threads that each push and then
pop. The queues: the shim's general queue (`shim_generic`, the segmented
queue, B0), its substitutes where one side is single (`shim_mpsc`, the
segmented queue's single-consumer form; `shim_spmc`, the ring whose
consumers take turns), and B1: moodycamel ConcurrentQueue, Boost's
lock-free queue, `std::mutex` around `std::queue`, and LCRQ, LPRQ, LSCQ and
the fetch-and-add array queue from the LPRQ paper's artifact; the retired
two-lock list (`orig_generic`) and the list it had for many producers
(`orig_mpsc`) for reference. This host has no oneTBB, so the 21 October
leg's oneTBB rows have no counterpart here.

Every row allocates one heap object per element, on different sides: the
shim keeps the element by value in its cell and its pop returns a
`shared_ptr`, allocated and freed by the consumer; the libraries carry a
pointer or `shared_ptr` the producer allocates and the consumer frees. A
free on another thread than the allocation costs more in most allocators,
so part of the shim's lead in `split` is likely that its allocation stays
on one thread; this is inferred, not measured.

16:59 to 18:09 local; every run passed its order and sum checks.
Frequency not fixed: median 4.10 GHz; the runs below 2 GHz (down to 0.25)
are `std_mutex` and `orig_generic`, whose threads sleep in the kernel,
which the user-cycles count reads as a lower clock. Outside the leg's set
the processors were busy 0.003 of the time at the median, 0.38 at most;
8 of 2,205 runs exceeded 0.10 (pairwise at 4 to 16 threads under glibc),
and dropping them changes no geometric mean below. Control drift 1.6%, no
trend. Widest row: `shim_generic` at 8:1 under jemalloc, 14.2 or about
25.7 M/s across repetitions (bimodal, one low run).

## What it shows

Paired ratios, median per configuration, geometric mean over shapes and
allocators with the range:

| comparison | shapes | ratio |
|---|---|---|
| shim_mpsc / shim_generic | 1:1, 2:1, 4:1, 8:1 | 1.29 (0.99-2.05) |
| shim_mpsc / moodycamel CQ | same | 2.35 (1.84-3.73) |
| shim_mpsc / orig_mpsc (the list of 21 October) | same | 2.30 (1.24-6.73) |
| shim_spmc / shim_generic | 1:1, 1:2, 1:4, 1:8 | 2.16 (1.62-2.88) |
| shim_spmc / moodycamel CQ | same | 2.11 (1.31-3.60) |
| shim_generic / moodycamel CQ | P = C, split | 1.67 (1.00-2.93) |
| shim_generic / LCRQ | P = C, split | 1.61 (0.70-3.94) |
| shim_generic / orig_generic | P = C, split | 6.15 (2.33-12.57) |
| shim_generic / orig_generic | 2:2, 4:4, 8:8 | 7.88 (4.66-12.57) |
| shim_generic / moodycamel CQ | pairwise, 1-16 threads | 1.05 (0.66-1.80) |
| shim_generic / LCRQ | pairwise | 0.85 (0.68-1.07) |
| shim_generic / LPRQ | pairwise | 0.89 (0.68-1.30) |
| shim_generic / LSCQ | pairwise | 1.18 (0.83-1.67) |
| shim_generic / fetch-and-add array | pairwise | 0.82 (0.71-1.03) |

- The general queue every unproved queue keeps is no longer the weak
  baseline of 21 October: 7.9× the two-lock list where both sides are
  many (2:2 to 8:8; 6.2× with 1:1), and ahead of moodycamel (1.24-2.49×) and LCRQ (1.09-2.67×) in
  every split shape with both sides many (geometric mean over allocators
  per shape; least at 8:8).
- In the pairwise workload it trails LCRQ, LPRQ and the fetch-and-add
  array queue from 4 threads on (0.71-0.85× at 4, 8 and 16 threads), and
  is level or ahead at 1 and 2; LSCQ is behind it (1.15× at 4 threads,
  1.18× overall).
- The substitutes still pay where one side is single: the mpsc form
  1.08-2.05× the general queue under glibc (most at 1:1, least at 8:1);
  the ring for many consumers 1.6-2.4×.

## How far it holds

- Microbenchmark: elements of 8 bytes and nothing between operations. W11
  (eval-pool) shows the ring for many consumers losing to the general
  queue with 8 workers and 1 µs tasks, a regime this leg does not cover.
- Every row allocates per element (above); the ratios between the shim's
  rows are free of that difference, those against the libraries are not.
- Frequency not fixed; no oneTBB on this host.

## What it means for the compiler

Where both sides are many, what the compiler leaves in place is now a
strong queue, not a straw man, so the unproved case loses little in the
split shapes; the pairwise gap to LCRQ, LPRQ and the fetch-and-add array
queue (up to 1.4×) is the remaining headroom there. Where one side is single the substitution still
pays (1.3× and 2.2× in the geometric mean), though less for many
producers than against the 21 October list, since the general queue it
replaces got stronger.

## Provenance

`meta.json`'s `git_sha` is empty: the leg ran from a copy of the tree on
the second host, not a git checkout. The runtime, shim, adapters and W9
are unchanged on main from 5a70189 to 7b40685 (compared file by file);
driven by campaign.sh at a19428f, which writes `unpinned.tsv` with a
header (run number, the first and last `runs.jsonl` line the run wrote,
user GHz, the ratio to reference cycles, nan on this host, user cycles,
task-clock, outside-busy share, allocator, command).
