# Known defects

State as of September 2026, from a review of the prototype before it moved
into this repository. Two items marked below have since been fixed; the rest
are open.

Read the first section before building anything on top of the analysis.

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
  as two. This one errs safe.

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
each claim carries the command behind it. The material is the diploma author's
own deleted example, recovered from git history, in which one queue is written
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

Still open: there is no end-to-end example from C++ source, the tests cover
only cases the code already handles, and one test uses unordered matching that
would accept two queues having their verdicts swapped.
