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
| as written | every queue `mpmc` | each array of queues is counted as one queue, every element together; the threads start in `Encode`, which only `main`, in another file, calls: from this file it may run any number of times |
| queues as globals (`DEDUP_FLAT`) | every queue `mpmc` | the same: `Encode` is called only from another file |
| the arrays kept, `main` in this file (`DEDUP_UNITY`) | with one thread per stage, refine, deduplicate, compress `spsc`; reorder `mpsc`; with threads per stage from `argv`, what a reader would say at any count | the union over each array's elements is what a reader would say, since every element of an array has the same threads |
| queues as globals, `main` in this file (`DEDUP_FLAT DEDUP_UNITY`) | the same as the arrays kept | what a reader would say |

Every verdict is sound: the reorder queue has two producing stages at any
thread count, and nothing proved wrong is specialised. The builds
locate dedup's two obstacles, arrays of queues and a program spread over
several files. Before arrays of queues were counted as one (26 Oct), the
build as written got no verdict at all: every queue was an element of an
array indexed at run time. In the one-file builds with one thread per
stage the three queues become inline rings, the tags an expert would
write by hand: the one pop, inside the helper
every stage calls, is checked as a pop of each queue it may act on, and
every stage copies its element out before the next pop. The reorder queue,
two stages into one, becomes the shim's list for many producers.

## The original, lifted

`build.sh` also builds PARSEC's own sources, unedited, through `wtc-cc`:
the encoder, the queue and `dedup.c`'s main as one unit (`--unity`), the
other files as PARSEC builds them. The queue is read as a C queue
(`--wtc-lift-c-queues`, docs/lifting.md "C queues") whose pops and
pushes are batched, and is refused: its operations assert on the queue's
state and on the elements (no more terminations than producers, no push
after the queue has finished, no null element), checks the substitute
does not make, so `dedup-lifted` keeps PARSEC's queue, as
`dedup-unlifted` does. Without the assertions it would lift (the model,
test/Integration/LiftedC/dedup.c): a batch taken or appended at one step
of the runtime, so the refine threads popping one queue get disjoint
adjacent runs, as under the mutex, with the verdicts of the port built
as one unit with the arrays kept (`dedup-compiler-unity`): the refine
queues spmc (one fragmenting thread, the refine threads a loop over the
thread count starts), the reorder queue mpsc (two stages into one),
deduplicate and compress mpmc.

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
