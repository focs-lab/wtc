# PARSEC ferret on WTC

Content-based image search from PARSEC: six pipeline stages (load, segment,
extract, vectorise, rank, output) connected by five queues. It is the
application the evaluation uses to ask what specialisation does to a real,
coarse-grained pipeline.

## Licence

This directory alone is under the GNU General Public License, version 2 or
any later version, because what it produces is derived from the Ferret
Toolkit, whose sources carry that notice (`LICENSE` is the licence text the
Ferret Toolkit ships). The rest of the repository is under the Apache
License 2.0 with LLVM Exceptions.

## What the port changes

`port.py` turns `benchmark/ferret-pthreads.c` of the pinned PARSEC tree
(bamos/parsec-benchmark, 2e492f771c6749577fe4e7610d516a9e7f8ad580) into a
C++ program on `wtc::queue`; its docstring lists every change. The queues
carry pointers to each stage's record, as the original's do; a consumer
waits by spinning on `try_pop`, as every variant measured does, and a queue
ends with one null per consumer from its last producer. The thread pool,
which lives in another translation unit, is replaced by direct
`wtc::spawn` calls, so the analysis can see the threads: with `FERRET_ONE`
one thread per stage, fixed when the program is compiled; otherwise
`argv[6]` threads per middle stage, as in PARSEC.

## What the compiler finds

With one thread per stage, four queues are proved single-producer
single-consumer with borrowing consumers and become inline rings; the
first, which the loader fills from inside a recursive directory scan, is
refused ("reached through recursion"). With a thread count chosen at run
time every queue keeps the general implementation, which is correct: a
spawn in a loop is many threads. Run-time thread counts are what the
conditional specialisation in the evaluation plan would address.

## Building and running

    benchmarks/apps/ferret/fetch-inputs.sh <parsec-dir> <inputs-dir>
    benchmarks/apps/ferret/build.sh <parsec-dir> <out-dir>
    cd <inputs-dir>/simlarge && <out-dir>/bin/ferret-compiler-one corel lsh queries 10 20 1 out.txt

Every build's output equals PARSEC's own (`ferret-orig`) byte for byte with
one thread per stage, and line for line with more. The leg `eval-ferret` in
`benchmarks/scripts/campaign.sh` measures them.

PARSEC's input site did not answer when this was written; `fetch-inputs.sh`
takes simlarge from a published image of the PARSEC tree pinned by digest
and checks the archive's checksum. Docker Engine 28 no longer pulls that
image's format; the script then says how to read the archive out of the
image with an older engine, and takes it as a third argument.
