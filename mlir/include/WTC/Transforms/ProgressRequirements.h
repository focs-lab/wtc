#pragma once

// What progress a function is held to, and why (docs/progress.md,
// "Requirements").
//
// A requirement is declared on a function by an annotation of
// shim/progress.hpp:
//
//   WTC_LOCK_FREE              nothing it reaches blocks;
//   WTC_WAIT_FREE              that, and every path is wait-free: no retry,
//                              no wait for data;
//   WTC_WAIT_FREE_WITHIN(k)    that, and its worst case is a number of steps
//                              no more than k under the counts proved;
//   WTC_PROGRESS_CRITICAL      wait-free, with a worst case (not only an
//                              amortised bound) that is a polynomial in the
//                              counts, and no first-use effects; and nothing
//                              delays the elements it waits for
//                              (WTCBatchPushes.cpp refuses to batch the
//                              pushes of a queue of the type it pops).

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mlir::wtc {

struct Requirement {
    // Strongest last.
    enum class Kind : uint8_t { lock_free, wait_free, critical };
    Kind kind = Kind::lock_free;
    // WTC_WAIT_FREE_WITHIN(k): the most steps the worst case may take.
    std::optional<uint64_t> within;

    // "lock-free", "wait-free", "wait-free within 100 steps",
    // "progress-critical".
    std::string describe() const;
    // Whether this requirement is at least as strong as another.
    bool covers(const Requirement &o) const;
};

// The requirement f declares, the strongest of its annotations; nothing
// when it declares none.
std::optional<Requirement> declaredRequirement(cir::FuncOp f);

// A function a requirement holds for, and where the requirement comes
// from. Besides the annotations, the program states two requirements
// without saying so:
//
//   a signal handler (a constant handler installed by signal(), or stored
//   into the sa_handler or sa_sigaction of the struct sigaction a call of
//   sigaction() is handed) interrupts its own thread anywhere, so what it
//   reaches must be async-signal-safe: lock-free, and no allocation, no
//   lock, no static-initialisation guard, no thread-local first use;
//
//   a real-time thread (SCHED_FIFO or SCHED_RR, set by
//   pthread_setschedparam or sched_setscheduler on the calling thread or on
//   a thread the module started, or by pthread_attr_setschedpolicy with
//   PTHREAD_EXPLICIT_SCHED on the attributes a pthread_create is handed)
//   never yields to a thread of lower priority on its processor: its paths
//   must be wait-free apart from waits for data, and a wait for data must
//   not spin.
//
// A policy that is not a constant is taken for a real-time one. A handler
// or a thread the analysis cannot name is reported as such, not checked.
struct RequirementRoot {
    enum class Source : uint8_t { annotation, handler, realtime };
    cir::FuncOp func;
    Requirement req;
    Source source = Source::annotation;
    Operation *origin = nullptr;  // the call that installs the handler or sets the policy
    std::string why;              // "a signal handler, installed by sigaction"
    bool inferred() const { return source != Source::annotation; }
};

class RequirementRoots {
public:
    explicit RequirementRoots(ModuleOp module);

    llvm::ArrayRef<RequirementRoot> roots() const { return roots_; }
    // Calls that install a handler or set a real-time policy on a thread
    // the analysis cannot name, with why.
    llvm::ArrayRef<std::pair<Operation *, std::string>> unresolved() const { return unresolved_; }

    // The first requirement (that `which` accepts, when given) whose
    // function reaches f by direct calls, calls through a pointer whose
    // targets are known, or, for a real-time thread, the start of a thread
    // that inherits its policy; with the calls on the way, outermost first.
    // Null when none does. Every requirement that reaches f is in
    // allReaching: a pass that must respect the strictest asks that.
    //
    // The passes that would make a path slower for another thread's sake
    // ask it: the delegation of critical sections and the batching of
    // pushes and of locks.
    const RequirementRoot *reaching(cir::FuncOp f, llvm::SmallVectorImpl<cir::CallOp> *chain = nullptr,
                                    llvm::function_ref<bool(const RequirementRoot &)> which = {}) const;
    llvm::SmallVector<const RequirementRoot *, 2> allReaching(cir::FuncOp f) const;

private:
    void addHandler(cir::FuncOp h, Operation *origin, llvm::StringRef how);
    void addRealtime(cir::FuncOp entry, Operation *origin, llvm::StringRef how);
    void addRealtimeCaller(cir::FuncOp caller, Operation *origin, llvm::StringRef how);
    static cir::FuncOp spawnedEntry(cir::CallOp call);
    void computeReach();

    ModuleOp module;
    std::vector<RequirementRoot> roots_;
    std::vector<std::pair<Operation *, std::string>> unresolved_;
    // For each function and each root that reaches it, the call it was
    // first reached by from that root (null for the root itself); and the
    // roots that reach each function, in order.
    llvm::DenseMap<std::pair<Operation *, unsigned>, Operation *> via_;
    llvm::DenseMap<Operation *, llvm::SmallVector<unsigned, 2>> rootsOf_;
    // The reverse call graph and the functions that start threads, built
    // when a thread's own priority is raised.
    llvm::DenseMap<Operation *, llvm::DenseSet<Operation *>> callers_;
    llvm::DenseSet<Operation *> entries_;
};

} // namespace mlir::wtc
