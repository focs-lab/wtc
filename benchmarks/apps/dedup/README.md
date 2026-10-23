# PARSEC dedup on WTC

Compression with deduplication from PARSEC: five pipeline stages (fragment,
refine, deduplicate, compress, reorder) connected by four kinds of queue. It
is the application the evaluation uses to ask what the analysis does with
a program it must refuse as written, and what it would take not to.

## Licence

This directory is under PARSEC's BSD-3-Clause licence (`LICENSE`, the text
of the PARSEC tree), because what it produces is derived from dedup. The
rest of the repository is under the Apache License 2.0 with LLVM
Exceptions.

## What the port changes

`port.py` turns `src/encoder.c` of the pinned PARSEC tree
(bamos/parsec-benchmark, 2e492f771c6749577fe4e7610d516a9e7f8ad580), the one
file of dedup with queues and threads, into a C++ program on `wtc::queue`;
its docstring lists every change. The queues carry chunk pointers, as the
original's do, and are still arrays on the heap, one element per group of
four threads, found by an index computed at run time. A consumer waits by
spinning on `try_pop`, then takes what else is there up to the original's
batch limit; a queue ends with one null per consumer from its last
producer. The other files are PARSEC's, compiled as C.

## What the compiler finds

| build | what the analysis says | why |
|---|---|---|
| as written | every queue unknown | the queue is an element of an array |
| queues as globals (`DEDUP_FLAT`) | every queue `mpmc` | the threads start in `Encode`, which only `main`, in another file, calls: from this file it may run any number of times |
| the same with `main` in this file (`DEDUP_UNITY`) | refine, deduplicate, compress `spsc`; reorder `mpsc` | what a reader would say |

Every verdict is sound: the reorder queue has two producing stages at any
thread count, and nothing proved wrong is specialised. The three steps
locate dedup's two obstacles, arrays of queues and a program spread over
several files, each an item of the evaluation plan. In the last build the
three queues become plain `spsc`, not inline rings: the one pop, inside
the helper every stage calls, acts on several queues of one type, and the
check that a consumer only borrows its element is not made per queue.

## Building and running

    benchmarks/apps/dedup/fetch-inputs.sh <parsec-dir> <inputs-dir>
    benchmarks/apps/dedup/build.sh <parsec-dir> <out-dir>
    <out-dir>/bin/dedup-compiler -c -p -t 4 -i <inputs-dir>/native/FC-6-x86_64-disc1.iso -o out.ddp

Every build's output equals PARSEC's own (`dedup-orig`) byte for byte, at
1, 2 and 4 threads per stage, and decompresses to the input. Every build
is timed by PARSEC's hooks, from the first thread started to the last
joined. The leg `eval-dedup` in `benchmarks/scripts/campaign.sh` measures
them.

PARSEC's simlarge input is in its input archive, whose site did not answer
when this was written, and the image the ferret port takes its input from
carries none for dedup. The native input is a public file, the first Fedora
Core 6 x86_64 install disc, which `fetch-inputs.sh` downloads from the
Fedora archive and checks against the archive's SHA-1 and a SHA-256
recorded in the script.

On it dedup is coarse-grained: chunks average 1.9 KB, about 370 000 of
them, and a chunk spends tens of microseconds in compression for each
queue operation it takes. In a first, untimed run every build was within
1% of the others; the leg is what measures it.
