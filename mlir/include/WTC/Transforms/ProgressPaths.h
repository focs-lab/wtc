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

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"

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

} // namespace mlir::wtc
