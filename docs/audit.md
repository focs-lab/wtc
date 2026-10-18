# Known defects

State as of September 2026, from a review of the prototype before it moved
into this repository. Items marked fixed below have been fixed since; the rest
are open.

Read the first section before building anything on top of the analysis.

## The ownership analysis was unsound in three ways — fixed

The analysis counts how many threads push to each queue and how many pop, and
prints one of SPSC, MPSC, SPMC or MPMC. Until October 2026, substituting a
specialised structure on the strength of that verdict was not safe:

- **A spawn inside a loop counted as one thread.** A thread id was handed out
  per static call site, so a worker pool came out single-producer. A spawn
  site now counts as possibly many threads when it is inside a loop or a CFG
  cycle, or in a function that may itself run more than once
  (`spawn-in-loop.mlir`, `spawn-in-helper-called-twice.mlir`).
- **The main thread was not a thread.** A push from `main`, or from a function
  `main` calls, was discarded. `main` now has a thread of its own
  (`main-thread.mlir`: two producers against two consumers, not one).
- **A parameter index was ignored.** Every access through a function's second
  queue parameter was attributed to its first. Each parameter is now bound to
  the argument at its own position (`two-queue-params.mlir`).

Two of the four precision problems are fixed as well:

- Two queues reaching the same function on the same thread no longer collapse
  into the first (`same-function-two-queues.mlir`).
- Thread contexts are computed to a fixed point rather than by recursion over
  a cache seeded with empty placeholders, so results no longer depend on
  traversal order (`mutual-recursion.mlir`).

Still open:

- Indirect calls are still not followed. They are no longer invisible,
  though: a function whose address is used for anything but a spawn, and a
  function nothing in the module calls, run on an "unknown" thread, and any
  queue they touch is classified UNKNOWN (`escaping-function-address.mlir`,
  `unresolved-thread-context.mlir`). That errs safe, at the cost of verdicts
  for programs built around function pointers, lambdas or `std::function`.
- A queue whose address is passed to code the module cannot see may be used
  there by any thread. Nothing tracks that escape yet.
- Thread lifetime is not modelled. Two threads that never overlap in time count
  as two. This one errs safe.

## The dialect carries no concurrency semantics

The four operations are declared without traits, interfaces, memory effects or
verifiers. There is no memory ordering, no progress guarantee, no linearisation
point. An operation currently amounts to a tagged call: one attribute holds the
original function's name, another its original type. Nothing is expressed that
CIR did not already say.

Typing was inconsistent: the queue operations constrained their operand to the
queue type, while `lock` and `unlock` accepted anything.

Fixed in October 2026, the mechanical part: `lock` and `unlock` take a
`!wtc.mutex`; push and pop check their values against the queue's element
type; `fallback_impl` must name a `cir.func` that implements the same
operation and takes the arguments lowering will pass; each op declares that
it reads and writes memory (`test/Dialect/invalid.mlir`). The concurrency
semantics proper are still open, and
`decisions/0003-concurrency-semantics-of-the-wtc-ops.md` proposes them.

## Lowering crashes on hand-written IR — fixed

All four lowering patterns unconditionally dereferenced an attribute that is
declared optional. Every ownership-analysis test constructs its operations
without it, so running the lowering pass over any of them crashed.

Fixed in October 2026: the pass names the op and the missing attribute
instead (`test/Transforms/LowerWTCToCIR/missing-attributes.mlir`).

## Lifting is fragile in predictable places

Operands were taken by index with no arity check. That is fixed: lifting
checks each call's shape and names the call when it is unexpected
(`test/Transforms/LiftCIRToWTC/arity-mismatch.mlir`).

Still open: the element type for a pop is recovered from the layout of
`std::shared_ptr`, as the first pointer reached through its leading members.
That covers libc++, where `T*` is shared_ptr's first member, and libstdc++,
where it is the first member of the `std::__shared_ptr` base (found by CI on
Linux). It is still an assumption about layout; a deviation produces a
diagnostic, not a crash. Shim functions
are matched by exact string comparison against a record name, which will not
survive templates or nesting.

## The analysis result went nowhere — fixed

The classification used to be printed to standard output and nowhere else,
so a subsequent pass could not read it, and the pipeline stopped exactly where
specialisation would begin.

Fixed: each queue's root, its `cir.global` or `cir.alloca`, now carries its
verdict as `wtc.queue_kind`, and so does each push and pop. An op that can
reach several queues carries the weakest of their verdicts
(`queue-kind-attribute.mlir`, `queue-kind-join.mlir`). Output order follows
the IR rather than a hash map, and the pass marks analyses preserved.

Still open: the report is plain standard output, not MLIR diagnostics.

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

The analysis read operand zero expecting a `cir.get_global` naming the thread
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

**Lifting no longer crashes on this** (October 2026). The callee marks the
hidden argument `llvm.sret`, so lifting reads the queue from the argument
after it and lifts to a `wtc.queue_pop` that writes through the slot; lowering
puts the slot back, attribute included
(`test/Transforms/LiftCIRToWTC/sret-try-pop.mlir`).

Nor does the analysis miss the spawns: it takes a spawn's entry from the
first argument that is a function's address, not from operand zero. On the
prototype's example, emitted for either target, it now finds all six threads
and gives the known answer, SPSC for `q1` and MPMC for `q2`.

**The hidden argument depends on the target.** Measured at the pin on an
AArch64 macOS host: the same source gives `try_pop` and `spawn` their record
results directly, with no hidden argument, and the old fixed indices happen
to be right there. The same file emitted with `-target x86_64-apple-macos14`
puts the slot first. So a pass that works on one machine can still read the
wrong operand on another; both shapes have to be handled, and both now are
in lifting.

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

Still open: there is no end-to-end example from C++ source, and the tests
cover only cases the code already handles. Fixed: `multi-queue.mlir` used
unordered matching that would have accepted two queues with their verdicts
swapped; each verdict is now checked against the line naming its queue.
