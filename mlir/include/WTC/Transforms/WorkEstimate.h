#pragma once

// How much work does this code do?
//
// A transformation that removes synchronisation between two operations can
// put work under what remains: coarsening runs the code between two
// critical sections with the mutex held, fusing a pipeline runs its stages
// on one thread. Whether that pays depends on how long the code in between
// takes, and W8 and W12 measured where it stops paying (decisions/0006).
// This is the estimate those thresholds are compared with: a count of
// units over ClangIR as the front end emits it, before optimisation.
//
//   - most operations count one unit: a load, a store, an arithmetic or
//     logical operation, a comparison, a select, a pointer step, a
//     floating-point conversion; constants, stack slots, the addresses of
//     globals and members, other casts, scopes, yields, lifetime marks and
//     branches' own operations count none;
//   - a division or remainder by a constant counts 2 (it becomes a
//     multiply), by anything else 40; a floating-point division, remainder
//     or square root 15; a transcendental function 200;
//   - reading the time-stamp counter (the intrinsic, or rdtsc or rdtscp in
//     inline assembly) counts 90; any other intrinsic 1, other inline
//     assembly 1 a statement;
//   - a copy of a record, and memcpy, memmove, memset, memcmp, memchr and
//     strncmp with a constant length, count 2 and one more per 8 bytes;
//     with a length not known here, or strlen and strcmp, there is no bound;
//   - the allocator (what the shared blocking facts call the allocator:
//     malloc and its family, realloc, free, the replaceable operator new
//     and delete, DPDK's heap) counts 100: an allocation served from the
//     thread's cache. A large block the allocator maps, or a realloc that
//     moves its block, costs more, and the estimate does not see it;
//   - a call counts one unit and its callee's body, the largest of the
//     bodies an indirect call may reach when they are all known (the
//     shared indirectCallees). There is no bound for a call into a
//     function without a body here that is none of the above, a body that
//     may be replaced at link time, an indirect call whose targets are not
//     known, a recursive call, or calls nested more than kMaxDepth deep;
//   - a branch counts the larger of its arms (an if, a ternary), a switch
//     every case, since one may fall through to the next: the worst case,
//     not the expected one;
//   - a for loop whose trip count is a constant (constantTripCount) counts
//     its condition once more than its trip count, and its body and step
//     that many times; any other loop has no bound, and neither has a
//     region with more than one block (a goto).
//
// A unit is a count, not a time. Units become nanoseconds through one
// constant per class of host, measured on W8's work, a dependent chain of
// xorshift-multiply rounds of 25 units each; decisions/0006 has the table.
// Most of the loads and stores ClangIR emits for locals vanish in the
// optimiser, and the constant absorbs them for code shaped like W8's.
// Code whose operations overlap better than a dependent chain takes less
// than its estimate; code that misses the cache takes more.

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mlir::wtc {

// An estimate: a number of units, or no bound, with the first reason found
// and the operation that gives it.
struct WorkEstimate {
    double units = 0;
    bool bounded = true;
    Operation *at = nullptr;
    // A clause, as a remark can end with it: "it runs a loop whose trip
    // count is not known here".
    std::string why;

    static WorkEstimate of(double units) {
        WorkEstimate e;
        e.units = units;
        return e;
    }
    static WorkEstimate unbounded(Operation *at, std::string why) {
        WorkEstimate e;
        e.bounded = false;
        e.at = at;
        e.why = std::move(why);
        return e;
    }

    // Both run, one after the other. An unbounded estimate keeps the first
    // reason.
    WorkEstimate &operator+=(const WorkEstimate &o) {
        if (!bounded) return *this;
        if (!o.bounded) return *this = o;
        units += o.units;
        return *this;
    }
    // One of the two runs: the larger.
    static WorkEstimate larger(const WorkEstimate &a, const WorkEstimate &b) {
        if (!a.bounded) return a;
        if (!b.bounded) return b;
        return a.units >= b.units ? a : b;
    }
    WorkEstimate times(uint64_t n) const {
        WorkEstimate e = *this;
        if (e.bounded) e.units *= static_cast<double>(n);
        return e;
    }
};

// The trip count of a for loop when the module fixes it: `for (i = c; i <
// b; ++i)` with c and b constants (literals, or globals declared constant
// with a value), and the comparisons <=, != and, counting down by one, >
// and >=. i is a stack slot that nothing writes but the loop's step, by
// one, and a store of the constant just before the loop. A loop whose
// bound i reaches only by wrapping (i <= the type's largest value, i >= its
// smallest) has no count. A break may end the loop sooner; the count is
// then a bound, which is what an estimate needs.
std::optional<uint64_t> constantTripCount(cir::ForOp loop);

class WorkEstimator {
public:
    static constexpr unsigned kMaxDepth = 32;

    explicit WorkEstimator(ModuleOp module) : module(module), layout(module) {}

    // Leaves out an operation, and what it holds, when it returns true.
    using Skip = llvm::function_ref<bool(Operation *)>;

    // One operation, and the regions it holds.
    WorkEstimate op(Operation *o, Skip skip = {});
    WorkEstimate region(Region &r, Skip skip = {});
    // One iteration of a loop: its condition, its body and its step.
    WorkEstimate iteration(cir::LoopOpInterface loop, Skip skip = {});
    WorkEstimate function(cir::FuncOp f);

private:
    ModuleOp module;
    DataLayout layout;
    llvm::DenseMap<Operation *, WorkEstimate> memo;
    llvm::SmallVector<Operation *, 8> stack;
    // Set when an estimate under way met the depth bound: what it finds
    // depends on where it was reached from, and is not remembered.
    bool metDepth = false;

    WorkEstimate call(cir::CallOp c);
    WorkEstimate external(cir::CallOp c, cir::FuncOp f);
    double own(Operation *o);
};

} // namespace mlir::wtc
