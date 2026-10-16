# Known defects

State as of September 2026 for the queue analysis, and October 2026 for the
layout passes (the section after "Still open"). The first review was of the
prototype before it moved into this repository; the sections below have since been worked
through, and each one now says what became of it. A defect is only marked
closed where a test would fail if it came back.

Read "Still open" and the October section before building anything on top of a verdict.

## Closed, with the test that keeps them closed

| Defect | What closed it | Test |
|---|---|---|
| A spawn inside a loop counted as one thread | thread multiplicity: a site inside a loop, or in a function that runs more than once, stands for many threads | `test/Integration/spawn_loop.cpp` |
| `main` was not a thread and its accesses were discarded | `main` and the functions global constructors run are thread roots | `test/Integration/main_pushes.cpp`, `unresolved-thread-context.mlir` |
| A parameter index was ignored, so a two-queue function credited everything to the first | parameters resolve through the call site of the instance, by index | `test/Integration/two_queues_one_worker.cpp` |
| Two queues on one thread through one function collapsed | same mechanism; there is no per-function cache to collapse them | `param-passed-multi-instance.mlir` |
| An unresolved access was dropped, turning "I don't know" into "nobody" | an unresolvable access poisons the queue's type; every queue of it becomes unknown | `unresolved-queue.mlir`, `test/Integration/queue_via_struct.cpp` |
| Function pointers and virtual calls were invisible | a function whose address is taken other than as a spawn entry is a root callable from any thread | covered by the same poison path |
| The spawn entry had to be a bare `cir.get_global` | the operand is traced through casts | `test/Integration/*` (the real shape) |
| Real `spawn` and `try_pop` were not understood at all | operands read through the calling convention; the hidden return slot is recognised | every `test/Integration` test |
| `try_pop` on real output segfaulted | `queue_pop` has a return-slot form, and a verifier that insists on exactly one form | `test/Integration/queue_example.cpp` |
| Lowering dereferenced an optional attribute and crashed | it diagnoses instead | `check-wtc` runs lowering over every analysis test |
| The verdict went to standard output and nothing could read it | it is an attribute on the IR, plus remarks | all of the above |
| Output order came from a hash map | insertion-ordered, and the two-queue test matches in order | `multi-queue.mlir` |
| No end-to-end example from C++ source | `examples/`, `tools/wtc-cc`, and seven `.cpp` lit tests | the tests themselves |
| A test used unordered matching that would accept swapped verdicts | ordered matching | `multi-queue.mlir` |

Two infrastructure defects were fixed before the move into this repository:
the build hard-coded one developer's home directory, and `shim/queue.hpp` did
not compile under any compiler because its move constructor was written
`= operator delete`.

## Still open

**One translation unit is assumed.** A second unit touching the same global
queue is invisible. Nothing enforces this; `wtc-cc` compiles one file.

**Queues reached through a member or the heap are `unknown`.** Tracing stops
at a load that is not from a stack slot written once. That is the safe
direction, and it is also the direction that gives up on a large class of
real programs. `test/Integration/queue_via_struct.cpp` pins the current
behaviour so that improving it is a visible change.

**The dialect still says nothing CIR did not.** The operations carry no
memory ordering, no progress guarantee and no linearisation point; the
topology attribute is the first fact in the dialect that CIR did not already
have. The static checks the vision describes need the rest.

**The ring's block size was chosen by measurement, not derived.** Below a few
thousand slots the ring has a slow regime; `runtime/spsc_ring.hpp` records
what was measured and why the default is what it is.

---

## The layout passes, October 2026

`--wtc-false-sharing-diagnose` and `--wtc-align-by-owner` were reviewed
four times after their numbers were taken on memcached, the third and
fourth times on hand-written programs with known answers. Every defect
found is closed with a test that fails on the code before the fix; the
list, with the tests, is in `docs/corpus.md`. The ones that changed
verdicts: the walk lost a thread spawned through a helper's local; a goto
made every access in its function "repeated"; a thread-local global, a
stack slot of a function called twice, two spawns in the two arms of an
`if`, and overlapping byte ranges all gave `definite`; atomic
read-modify-writes and bit-field stores were not memory accesses at all; a
recursive function ran "once"; a thread with an access the walk could not
place still made a pair cold, which on memcached was 349 pairs; the
alignment pass raised the alignment of declarations, sections,
thread-local and weak globals; a goto in a caller, or in `main`, still
made single writes hot; a once-only read of a whole record widened the
same thread's once-only write; the escape check called every slot
published; and the start-up phase ended at a spawning callee's call rather
than inside it.

The heap-and-element round of October (heap objects by allocating call,
array elements by constant index or by the spawn loop's own, one element
per thread, elements the analysis cannot tell apart) was reviewed the same
way: 33 defects by three readings, the verdict-changing ones being a
loop-spawned thread whose every access was "repeated" even on the element
it owns alone, five ways a loop can hand an element to no thread or to
several while the analysis claimed one thread per element, a heap object
with two names and so two roots, a pointer global written through a
`global_view` or an alias, an entry lost through an integer round trip,
and a recurrence left unexpanded whose reach was treated as seen. All are
closed in `docs/corpus.md`'s section on heap objects and elements.

Still open for these passes, stated here and in `docs/corpus.md` rather
than in the remarks themselves:

**No notion of time.** A `definite` verdict says two accounted-for threads
repeatedly touch the two fields, not that they do so at the same time. A
spawn, a join, and a second spawn of the same worker are two threads to the
walk. This errs *unsafe* for a false-sharing verdict: a pair reported
between threads that never overlap costs nothing.

**No notion of a lock.** Accesses under one mutex are not seen as
serialised. On memcached a shared lock did not decide a pair by itself;
who writes, hand-offs and rate did.

**No notion of rate.** Repeated or once is all the walk can tell; a command
handler that runs a thousand times a second and one that runs once a day
are both "repeated". Every memcached pair was decided by rate, by hand.

**Objects it cannot name.** A field reached through a pointer the walk
cannot follow -- loaded from a field of another object, from a global
written twice, from a `realloc` -- is counted as unidentified in every
summary, never guessed at: 4,377 accesses on memcached against 1,491
pairs. Heap objects are named by the call that allocated them and array
elements by a constant index or the spawn loop's own; an element at any
other index is placed on its array without knowing which, counted apart
(678 on memcached), and every pair there is at most possible. Objects from
two allocations are never neighbours to the analysis, whatever the
allocator does: Phoenix `histogram`'s known false sharing, between the
per-thread histograms each worker allocates, is out of its reach.

**Placement across objects** is the linker's and the allocator's. The
analysis reports where fields lie within an object; which objects share a
line is checked with `nm`, one pair at a time.

# The original review

What follows is the review as first written, kept because the reasoning
matters and because the table above refers to it.

## The ownership analysis is unsound in three ways

The analysis counts how many threads push to each queue and how many pop, and
prints one of SPSC, MPSC, SPMC or MPMC. Substituting a specialised structure on
the strength of that verdict is not yet safe.

**A spawn inside a loop counts as one thread.** A thread id is handed out per
*static* call site, not per dynamic spawn. A loop that starts N producers
yields one id, and the queue is classified single-producer. That is the common
worker-pool shape, and getting it wrong introduces a race rather than merely
losing performance.

**The main thread is not a thread.** Only functions passed to the spawn shim
receive a context. A push from `main`, or from a function `main` calls
directly, is discarded with a diagnostic and never counted. If `main` pushes,
one worker pushes and two pop, the verdict is one producer against two
consumers when the truth is two against two.

**A parameter index is ignored.** Resolving a queue held in a function
parameter checks only that the block argument belongs to the function, never
which parameter it is, and takes operand zero of the call regardless. For a
function taking two queues, every access to the second is attributed to the
first.

Three further problems cost precision rather than correctness:

- Two queues reaching the same function on the same thread collapse: the search
  takes the first instance with a matching thread id and stops, so the second
  queue records no accesses at all.
- The recursion guard writes an empty value into the cache before computing,
  and entries derived from a poisoned one stay incomplete permanently. Results
  depend on traversal order.
- The call graph is built from symbol references, so function pointers,
  lambdas, `std::function` and virtual calls are invisible, silently.
- Thread lifetime is not modelled. Two threads that never overlap in time count
  as two. For the queue analysis this errs safe; for a false-sharing verdict
  it does not (see the October section).

## The dialect carries no concurrency semantics

The four operations are declared without traits, interfaces, memory effects or
verifiers. There is no memory ordering, no progress guarantee, no linearisation
point. An operation currently amounts to a tagged call: one attribute holds the
original function's name, another its original type. Nothing is expressed that
CIR did not already say.

Typing is inconsistent: the queue operations constrain their operand to the
queue type, while `lock` and `unlock` accept anything.

## Lowering crashes on hand-written IR

All four lowering patterns unconditionally dereference an attribute that is
declared optional. Every ownership-analysis test constructs its operations
without it, so running the lowering pass over any of them crashes. This is not
hypothetical: a future specialisation pass will emit operations the same way.

## Lifting is fragile in predictable places

Operands are taken by index with no arity check. The element type for a pop is
recovered by casting the result type to a record and taking field zero, which
hardcodes an assumption about `std::shared_ptr`'s layout; a deviation produces
a crash rather than a diagnostic. Shim functions are matched by exact string
comparison against a record name, which will not survive templates or nesting.

## The analysis result goes nowhere

The classification is printed to standard output. Nothing is written into the
IR, no MLIR diagnostics are used, and analyses are not marked preserved. A
subsequent pass cannot read the result, so the pipeline stops exactly where
specialisation would begin. Output order is also unstable, because the results
are held in a hash map.

## The path from real C++, measured

Until September 2026 every test was hand-written IR and the passes had never
seen compiler output. They have now. What follows is measured, not predicted;
each claim carries the command behind it. The material is the prototype's own
deleted example, recovered from git history, in which one queue is written
and read by one thread each and a second is written and read by two each, so
the correct verdict is known in advance.

```
clang++ -std=c++17 -I shim -fclangir -emit-cir queue_example.cpp -o qe.cir
wtc-opt qe.cir --lift-cir-to-wtc
```

### Annotations survive, at every optimisation level

The earlier worry was that the shim functions, being `inline`, would be
substituted away along with the annotations the lifting pass looks for. They
are not. All four annotations are present on `cir.func` at `-O0` and at `-O1`,
and all six calls to the spawn shim remain in both.

The reason is structural rather than lucky: `-emit-cir` stops before LLVM IR,
and inlining happens in the middle end afterwards. At `-O0` the functions carry
`no_inline`, at `-O1` `inline_hint`, and in neither case has anything been
substituted yet. This risk can be struck off.

### Returning by value shifts every operand index

This is the one that breaks everything, and it breaks in two places.

`wtc::spawn` returns `std::thread` by value. A non-trivial return type is
passed as a hidden first pointer argument, so the call is

```
cir.call @_ZN3wtc5spawnEPFvvE(%ret_slot, %fn) : (!cir.ptr<!rec_std..thread>, !cir.ptr<!cir.func<()>>) -> ()
```

The analysis reads operand zero expecting a `cir.get_global` naming the thread
entry point. Operand zero is the return slot, so the lookup fails and the call
is skipped. All six spawns are skipped, no thread ever gets an identifier, and
the analysis prints nothing at all.

`queue::try_pop` returns `std::shared_ptr<T>` by value, with the same
consequence and one more: the CIR call now returns **void**, because the result
travels through the hidden argument. The lifting pass calls `op.getResult()` on
it, which on a zero-result operation is not a diagnostic but a segmentation
fault. Lifting the queue example crashes inside `LiftQueueCallPattern`.

`queue::push` is unaffected: it returns void already, so the queue really is
operand zero there.

### The mutex path works, start to finish

With no return values in play, everything lines up. The record type is named
exactly `wtc::mutex`, which is what the type converter compares against;
the annotations are in place; `lock` and `unlock` take the mutex as operand
zero and return void. Lifting the mutex example produces two dialect
operations. That is the first time any part of this project has consumed real
compiler output.

### What this adds up to

Half the pipeline is sound and the other half is blocked by one cause. Every
queue-side failure traces to operands being read by fixed index when the
calling convention put something else there. The shape of a fix is visible
from here: read arguments through the interface that already accounts for
this, and treat a call with no result as a case to handle rather than one to
assume away. None of that is attempted in this document; it is the work, not
the diagnosis.

## Infrastructure

Fixed before the move into this repository:

- The build hardcoded one developer's home directory for the Clang and ClangIR
  headers, and linked five static libraries by absolute path.
- `shim/queue.hpp` did not compile under any compiler: the move constructor was
  written `= operator delete`.

Those three were the sharpest observation in this document and all three are
now closed: there is an end-to-end example from C++ source, the tests cover
cases the code got wrong rather than only the cases it handled, and the
unordered match is ordered. See the table at the top.
