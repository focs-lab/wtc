# g3log on WTC

g3log is an asynchronous logger. Each `kjellkod::Active` object owns a
`shared_queue<std::function<void()>>`, a `std::queue` under a mutex with
a condition variable, and starts one thread that pops and runs the
closures; the logger's worker is one such object, and each sink another.
The leg builds the logger and its performance test's statement as one
unit, so that the analysis sees the program whole, and lifts the queue
class without a source edit (docs/lifting.md).

## Version and licence

The pin is release 2.6.2, commit f7014e42e48d52db0c4c973658a8027dbf7a576e;
`fetch.sh` checks it. g3log is in the public domain (the Unlicense,
`LICENSE`); so is this directory.

## The builds (`build.sh`)

| build | what it is |
|---|---|
| `g3log-orig` | the sources as released, as one unit, `clang++-18 -O2` |
| `g3log-generic` | the same through `wtc-cc --no-specialize`: lifted, the general-purpose queue |
| `g3log-compiler` | the same through `wtc-cc`: lifted, the compiler's choice |
| `g3log-port` | the hand port (`port.py`: `shared_queue` over `wtc::queue`) through `wtc-cc` |
| `g3log-port-mpsc` | the hand port with the queue declared mpsc by hand |

`bench.cpp` runs the performance test's `LOG(INFO)` statement on THREADS
threads into one worker with a null sink or the default file sink.

## What the compiler finds

The queue class lifts. Its verdict is many to many: `queue_ in any
shared_queue<std::function<void ()>>` stands for the queue of every
`Active` object, and the program builds one per sink type it
instantiates and one for the worker, five in this unit, each with its
own consumer thread. Each object's queue has one consumer, its own
thread, which a programmer knows and the hand-declared build says; the
analysis would need to show that the thread an object starts pops only
that object's queue, which it does not read through `std::thread`'s
member-pointer form. The reduced model in
`test/Integration/Lifted/g3log.cpp` has one such object, and there the
queue is proved mpsc.

Two things kept the real program from building as it does now: the
worker's constructor, defined out of line, is called through the
front end's alias of it, which read as a call of a function with no
body (fixed 30 Oct 2026, so the queue is no longer unknown), and a
reference to a function was lowered with `dereferenceable(0)`, which
the IR verifier rejects (`tools/wtc-cc` drops it).

## It does not run yet

Every build through ClangIR, `--no-lift` included, fails at run time: a
sink thread takes SIGSEGV inside the `std::function` that calls the
sink's member function, and g3log's own crash handler then waits for
ever. `g3log-orig`, the same unit built by clang++, runs. The fault is
in ClangIR's code for this program, not in the lifting, and the leg's
numbers wait for it; `benchmarks/scripts/lift_attribution.sh` leaves
g3log out.
