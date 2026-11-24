#pragma once

// The progress of the program's own loops (docs/progress.md, "Loops").
//
// The progress check reached every blocking call and judged no loop: a
// loop that spins on an atomic until another thread sets it reaches
// nothing blocking and is still not lock-free. A loop is classified here
// by what its exit depends on:
//
//   counted   a `for` whose trip count the program fixes (loopTrips in
//             Utils.h): it multiplies its body's bound, and its level is
//             its body's;
//   retry     the exit depends on a compare-and-swap that failed because
//             another thread changed the location since it was read:
//             lock-free, with no bound; a single-writer retry (the location
//             is a local that no other thread can name) never fails and
//             runs once;
//   wait      the exit depends on what another thread writes, read by an
//             atomic or volatile load, an exchange or a fetch-and-op, or a
//             compare-and-swap of an expected value the loop sets itself
//             (a spin lock): blocking;
//   dual      the exit depends on whether a queue had an element: a wait
//             for data, which is the request of a dual operation
//             (partial), not a wait for another operation in progress;
//   unknown   anything else: the exit depends on the thread's own data
//             only. Finite, no other thread can stretch it, and no number
//             can be printed;
//   forever   no exit at all: a constant true condition and nothing that
//             breaks out or returns. It does not complete, which no
//             requirement allows; one that polls a queue on the way is a
//             dual wait that never ends, one that reads what other threads
//             write a wait.
//
// A retry is read only where the loop's exit depends on nothing but its
// compare-and-swap, made in the loop itself or by a member of std::atomic,
// and nothing it read decides whether it skips the compare-and-swap (a
// test-and-test-and-set continues, or branches around it, while the word
// is held): anything else is a wait. A compare-and-swap inside a helper of
// the program's (a try_lock) cannot be told from a test-and-set and counts
// as a spin lock.
//
// What the exit depends on is the backward slice of the loop's condition
// and of the conditions under which it breaks or returns, through the
// local variables the loop writes and the calls it makes (a call's result
// depends on whatever its callee reads). A plain load is not evidence of a
// wait: in a program without data races no other thread writes, while the
// loop runs, a location the loop reads without synchronising, so such a
// loop's exit depends on the thread's own data. That is an assumption,
// listed in docs/progress.md.

#include "WTC/Progress.h"
#include "WTC/Transforms/Blocking.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

#include <memory>
#include <optional>
#include <string>

namespace mlir::wtc {

enum class LoopKind { Counted, Retry, SingleWriter, Wait, Dual, Unknown, Forever };

struct LoopClass {
    LoopKind kind = LoopKind::Unknown;
    Count trips = Count::unbounded();  // Counted: the trip count; SingleWriter: one
    std::string why;                    // in words, for a note
    Operation *evidence = nullptr;      // what the exit depends on, when there is one
};

class LoopClassifier {
public:
    explicit LoopClassifier(ModuleOp module) : module(module) {}

    // A cir.for, cir.while or cir.do.
    LoopClass classify(Operation *loop);
    // A goto: control flow the regions do not show, which may be a loop.
    // Classified as a loop whose body is the whole function.
    LoopClass classifyGoto(cir::GotoOp go);

    // The level a loop of this class gives its path.
    static Level levelOf(const LoopClass &c);

private:
    // What a value may depend on, as flags.
    enum Ev : unsigned {
        evCas = 1,        // a compare-and-swap's result
        evRead = 2,       // an atomic or volatile load
        evPop = 4,        // whether a queue had an element
        evRmw = 8,        // an exchange, a test-and-set or a fetch-and-op's result
        evConstCas = 16,  // a compare-and-swap of an expected value the loop sets itself
        evHelperCas = 32, // a compare-and-swap inside a helper of the program's, not readable as a retry
    };
    struct Evidence {
        unsigned flags = 0;
        Operation *first = nullptr;  // the first operation found for each kind, in order of the flags
        Operation *cas = nullptr;
        Operation *read = nullptr;
        Operation *pop = nullptr;
        Operation *rmw = nullptr;
        Value casObject;             // the object of the compare-and-swap, where it is a call's `this`
    };

    unsigned summary(cir::FuncOp f);
    // `residual`: what the value depends on other than through a
    // compare-and-swap, which is then not followed. A retry is a loop whose
    // exit depends on its compare-and-swap alone.
    void slice(Operation *loop, Value v, Evidence &ev, llvm::DenseSet<Value> &seen, bool residual = false);
    // The exits of a loop: its condition, the conditions under which it
    // breaks out, returns or continues, as evidence; `none` when it has no
    // exit at all (a constant true condition and nothing that leaves).
    void exits(Operation *loop, Evidence &ev, bool residual, bool &none);
    bool metCycle = false;
    void note(Evidence &ev, unsigned flag, Operation *op);
    bool setsConstantInside(Operation *loop, Value slot);
    bool singleWriter(Value object);
    LoopClass decide(const Evidence &ev, Operation *at);

    ModuleOp module;
    llvm::DenseMap<Operation *, unsigned> summaries;
    llvm::DenseSet<Operation *> inProgress;
};

// The progress of a path: what a function guarantees, composed from what
// it runs (docs/progress.md, "Composing a path").
//
//   in sequence   the level is the weaker, the steps add;
//   alternatives  (the arms of an if, a ternary, the cases of a switch)
//                 the level is the weakest, the steps are the larger,
//                 termwise;
//   a loop        by its class: a counted loop multiplies its body's steps,
//                 a retry makes the path lock-free without a bound, a wait
//                 makes it blocking, a dual wait partial, and a loop over
//                 the thread's own data leaves its steps finite but not
//                 read, unless its body takes none;
//   a call        the callee's composition, once per function; a call
//                 into a function without a body here is read as the
//                 progress check reads it (Blocking.h): one that touches
//                 only its arguments takes no step, any other blocks;
//   a wtc op      its row of the table under the counts proved for its
//                 object (wtc.producers_count, wtc.consumers_count on the
//                 queue's root), with the element's special members it
//                 runs composed as calls.
//
// Steps are the synchronising ones: the operations on wtc objects, by
// their rows, and one for each atomic access of the program's own code.
// Plain computation takes none.
struct PathProgress {
    Level level = Level::wait_free;
    bool partial = false;
    Bound bound = Bound::zero();
    Bound amortised = Bound::zero();  // the amortised bound where a row has one, else the worst case
    Alloc alloc;
    Kernel kernel = Kernel::none;
    unsigned firstUse = first_use_none;
    // The step that made the level what it is, and why; the step that made
    // the bound what it is when it is not a polynomial.
    Operation *weakest = nullptr;
    std::string weakWhy;
    // The counts the steps were instantiated with, in words, for a remark:
    // "np = 3 for @q".
    llvm::SmallVector<std::string, 2> counts;
};

class PathComposer {
public:
    explicit PathComposer(ModuleOp module);
    PathProgress of(cir::FuncOp f);
    // The counts of the object a wtc operation acts on, from its queue's
    // root; unknown when it may act on several, or its queue has none.
    ThreadCounts countsOf(Operation *op) const;
    // The queue a wtc operation acts on, by the name its root gives it.
    std::string queueName(Operation *op) const;
    LoopClassifier &loops() { return classifier; }

private:
    PathProgress op(Operation *op);
    PathProgress region(Region &r);
    PathProgress call(cir::CallOp call);
    PathProgress loop(Operation *loop);
    PathProgress enter(cir::FuncOp f);
    static PathProgress seq(PathProgress a, const PathProgress &b);
    static PathProgress alt(PathProgress a, const PathProgress &b);
    static PathProgress blocked(Operation *at, Level level, std::string why);

    ModuleOp module;
    LoopClassifier classifier;
    llvm::DenseMap<int64_t, Operation *> roots;  // queue_id -> root
    llvm::DenseMap<Operation *, PathProgress> memo;
    llvm::DenseSet<Operation *> inProgress;
    // Set when a composition met a function still being composed further
    // up: what it found is provisional and is not remembered.
    bool metCycle = false;
    ElementMembers elements;
    // Stack slots holding the shared_ptr of a borrowed pop of an inline
    // queue, which owns nothing: its members run nothing, as the progress
    // check reads them (BlockingQuery).
    llvm::DenseSet<Value> ownerlessSlots;
};

} // namespace mlir::wtc
