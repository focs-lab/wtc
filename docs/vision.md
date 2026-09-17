# Motivation and intended transformations

## The problem

Software that has to be fast and concurrent has two options today, and both
are bad.

The first is a general-purpose library: the standard library, Intel oneTBB,
Meta Folly, libcds. These are correct and well tested, and they are written for
the worst case, because the library cannot know anything else. A queue must
assume many producers and many consumers, so it pays for compare-and-swap loops
and memory barriers that a particular program may not need at all. Using a
multi-producer, multi-consumer queue for a flow that has exactly one producer
and one consumer is the canonical example, and it is extremely common.

The second is hand-tuning: writing the lock-free structure yourself, choosing
the padding, pinning the allocation to the right memory node. This gets the
performance and produces code that is fragile, unsafe and tied to one machine.

Compilers do not help, because to a compiler a synchronisation call is an
opaque barrier. It cannot hoist it out of a loop, fuse two of them, or
substitute a cheaper one, because it has no idea what the call means or what it
protects. It must be conservative about everything, and so it is.

The missing piece is context. A library sees one call site at a time. A
compiler sees the whole program: which threads exist, which of them touch which
object, how often each structure is read versus written, and what machine the
result will run on. That is exactly the information needed to choose an
implementation, and nothing in the current stack puts it where it can be used.

## Approach

Preserve the meaning of a synchronisation operation far enough down the
compilation pipeline that analysis can still act on it, then use that analysis
to choose code rather than merely to report problems.

Four layers:

**A shim library.** Header-only C++ providing `wtc::mutex`, `wtc::queue<T>` and
so on. Its job is not to implement anything clever. Its job is to be a
*recognisable* surface: a vocabulary the compiler can identify. Without the WTC
passes it compiles down to the ordinary standard-library primitives, so a
program that uses it stays portable and keeps working.

**Lifting through ClangIR.** Clang builds CIR, an MLIR-based representation
that still has the shape of the source in it. A pass recognises calls into the
shim and replaces them with first-class operations in the `wtc` dialect, so
that a push is a push and not a call to a mangled symbol.

**Analysis and transformation on the dialect.** Prove facts: how many distinct
threads push to this queue and how many pop from it; is this structure read far
more often than written; do two threads that share a lock sit on different
sockets. Then rewrite accordingly.

**Lowering.** Emit the chosen implementation. When nothing could be proved,
emit the general-purpose one. Degrading to the safe choice must always be
available, and must be the default when analysis is uncertain.

The last point is the load-bearing one. An analysis that is merely usually
right is worse than no analysis, because substituting a single-producer
structure into code that actually has two producers does not run slower, it
races.

## Transformations

### Specialising concurrent data structures

**By producer and consumer count.** Establish how many threads push and how
many pop. One of each permits a wait-free ring buffer with a release store and
an acquire load instead of a compare-and-swap loop. Intermediate cases have
their own cheaper algorithms. This is the first transformation worth building,
because it is the easiest to prove and the easiest to measure.

**By read-to-write ratio.** A structure that is overwhelmingly read and rarely
written does not want a reader-writer lock, which still bounces a cache line
between readers. It wants read-copy-update: readers proceed without
synchronisation, a writer publishes a new copy, and reclamation waits until no
reader can still be looking at the old one.

### Synthesising locks

**Topology-aware.** On a multi-socket machine a flat lock ping-pongs its cache
line across the interconnect. A hierarchical lock, where threads first contend
locally and only the local winner goes for the global lock, keeps most of the
traffic inside a socket. Which hierarchy is right depends on the machine, which
the compiler can be told.

**Biased towards an owner.** An object accessed almost always by one thread and
occasionally by another does not need a symmetric lock. The owner can take a
fast path that is a plain load and a plain store, with the expensive path
reserved for everyone else.

**Backoff matched to the critical section.** Spinning is right for a critical
section of a few instructions and catastrophic for one that does input or
output. The compiler can estimate the weight of the section it is protecting
and choose between spinning, yielding and parking, instead of using one
hardcoded constant everywhere the way libraries must.

**Combining under heavy contention.** Past a certain number of contending
threads, having each one take the lock in turn is worse than having them
publish their requests and letting a single thread execute the batch while the
data stays in its cache. A compiler can do this better than a library can,
because it can inline the user's critical section into the combiner's loop
instead of calling through a function pointer.

### Safe memory reclamation

Lock-free structures need a scheme for freeing memory that another thread might
still be reading: hazard pointers, epochs, read-copy-update. These schemes are
correct and expensive, and most of the expense is in the wrong place. A
traversal that protects every node it visits pays a memory barrier per
iteration; if the traversal is read-only and the protection is invariant across
it, the boundary belongs outside the loop. Establishing that it is invariant is
an aliasing question, which is a compiler's job.

The reclamation schedule is also a compiler's business. Freeing in large
batches, which epoch-based schemes do naturally, defeats the allocator's own
amortisation and shows up as latency spikes under bursty load. Spreading the
frees out fixes it, and the right spacing depends on the program.

### Micro-architectural tuning

**Padding.** False sharing between two counters that different threads write is
invisible in the source and costly at run time. The right amount of padding is
a property of the target processor, not of the source, so `alignas` with a
hardcoded constant is guesswork. The compiler knows the target.

**Allocation placement.** Standard allocation is blind to which thread will own
the memory. Knowing the consumer's thread and its memory node lets allocation
land where it will be read, and lets two objects owned by different threads
land on different cache lines even when they are separate heap allocations,
which `alignas` cannot express.

**Prefetch.** When the analysis shows that a consumer reads data immediately
after a producer hands it over, the producer can warm the consumer's cache.

### Pattern-driven rewriting

**Batching.** Pushing in a loop takes the synchronisation cost once per
element. Reserving space once, writing the elements plainly, and publishing
once takes it once per batch. This changes the latency profile, because the
first element now waits for the last, so it must be opt-in rather than
automatic.

**Recycling instead of allocating.** The request-response shape, where an
object is allocated by one thread, handed through a queue, used and freed by
another, defeats ordinary escape analysis: the object escapes, so it must go on
the heap, and the allocator becomes a contention point. But the lifecycle is
completely visible. A private recycling buffer between exactly those two
threads turns dynamic allocation into circular reuse.

**Local buffering.** A worker that pulls tasks from a shared queue and pushes
sub-tasks back into it contends with every other worker twice per task. A
thread-local buffer checked first absorbs most of that, which is what
work-stealing runtimes do, except that here it can be injected into existing
code instead of requiring the program to be rewritten against a runtime.

## Static checks

The same representation supports checks a dynamic tool cannot make, because
they are about intent rather than about a particular execution.

**Declared topology.** A queue annotated as single-producer, used by two
producing threads, is a bug that should stop the build and name both call
sites, not one that shows up as corruption in production.

**Progress guarantees.** A region declared lock-free must not reach a blocking
call. That is a reachability question over the call graph, answerable
statically, and the answer is far more useful as a call chain than as a
mysterious latency spike. Calls into unannotated external code are assumed
blocking.

**Bounded waiting.** Lock-freedom guarantees that *someone* progresses, not
that *this thread* does. A thread can spin in a failed compare-and-swap loop
indefinitely, which is exactly the tail-latency behaviour that matters in the
domains this project targets. Where the compiler synthesised the
synchronisation, it has the control-flow graph and can bound the work on the
critical path, or refuse.

**Cross-domain access.** Reading an atomic owned by a thread pinned to another
socket is not incorrect, it is just slow in a way nothing reports.

## Relation to existing work

Library research has mostly pursued better universal algorithms: more
sophisticated multi-producer queues, wait-free constructions, improved
reclamation schemes. The results are real and they do not address the problem
here, which is that a universal algorithm is the wrong thing to run when the
program is not universal.

Program synthesis from specifications attacked the problem from the other end
and has struggled to scale to production-sized data structures.

Static analysis tools find concurrency bugs and stop there, by design: they
verify, they do not transform.

Type systems that prevent data races, as Rust's ownership model does, address
correctness rather than the cost of being conservative, and say nothing about
false sharing, lock placement, or whether the chosen algorithm fits the use.

What is different here is using analysis, which the field has treated as a
verification tool, to drive substitution. That requires keeping enough meaning
in the intermediate representation to reason about, which is what MLIR makes
practical and what previous attempts lacked.

## What exists today

One slice, incomplete. The dialect has four operations and two types. Lifting
and lowering round-trip on hand-written IR. An ownership analysis classifies
queues by producer and consumer count and prints the result.

No transformation is implemented. Nothing is measured. The analysis has known
unsound cases, listed in `audit.md`, and must not be built upon until they are
closed.

The next honest step is not another pass. It is to write a specialised ring
buffer by hand, measure it against the general-purpose queue on a workload that
matters, and find out how large the prize actually is.
