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

## Two risks in the path from real C++

Neither shows up today, because every test is hand-written IR.

The shim functions are declared `inline`. At any optimisation level the call
disappears along with the annotation the lifting pass looks for, so lifting
only works at `-O0`.

The spawn shim returns `std::thread` by value while the analysis expects a
function pointer in operand zero. For a non-trivial return type the ABI
generally inserts a hidden first argument for the return slot, which shifts
every index. The tests use a spawn that returns void.

## Infrastructure

Fixed before the move into this repository:

- The build hardcoded one developer's home directory for the Clang and ClangIR
  headers, and linked five static libraries by absolute path.
- `shim/queue.hpp` did not compile under any compiler: the move constructor was
  written `= operator delete`.

Still open: there is no end-to-end example from C++ source, the tests cover
only cases the code already handles, and one test uses unordered matching that
would accept two queues having their verdicts swapped.
