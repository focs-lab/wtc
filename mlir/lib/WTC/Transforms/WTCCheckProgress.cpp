#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCInterfaces.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Demangle/Demangle.h"

#include <optional>
#include <string>

using namespace mlir;
using namespace mlir::wtc;

// Holds a function declared lock-free (WTC_LOCK_FREE, shim/progress.hpp)
// to its promise: nothing it can reach may block. That is a reachability
// question over the call graph, and the answer is far more use as a call
// chain than as a latency spike in production.
//
// What blocks (decisions/0004):
//   - a wtc operation whose implementation, as recorded on it, can wait
//     for another thread or may call the allocator (ProgressOpInterface);
//   - a call to a function without a body here that is known to block or
//     allocate: the allocator, the threading library's waits, input and
//     output, sleeping, throwing;
//   - any other function without a body here, and any indirect call whose
//     targets are not known: what cannot be seen is counted as blocking;
//   - inline assembly.
// A call to a function with a body is followed into it. A loop that spins
// on an atomic waiting for another thread is not judged: the check is a
// necessary condition for lock-freedom, not a proof of it.
//
// One exception, and it is what makes a consumer of an inline queue pass:
// the destructor of the shared_ptr a pop returns, when the pop is borrowed
// and its queue holds elements inline, frees nothing -- the shared_ptr
// owns nothing -- and is not followed.

namespace {

struct Finding {
    Operation *leaf;
    std::string why;
    llvm::SmallVector<Operation *, 8> chain;  // the calls from the function to the leaf
};

bool startsWith(const std::string &s, llvm::StringRef p) { return llvm::StringRef(s).starts_with(p); }

// Functions without a body here that cannot block: they touch only the
// memory they are given.
bool harmlessExternal(llvm::StringRef name) {
    static const llvm::StringSet<> names = {
        "memcpy", "memmove", "memset", "memcmp", "strlen", "strcmp", "strncmp",
        "abs", "labs", "llabs", "sqrt", "sqrtf", "fabs", "fabsf"};
    return names.contains(name);
}

// Why a function without a body here blocks, by its name. Known offenders
// get their reason; everything else gets the default, since a function
// that cannot be seen into cannot be cleared.
std::string externalReason(llvm::StringRef mangled) {
    const std::string name = llvm::demangle(mangled.str());
    static const llvm::StringSet<> alloc = {"malloc", "calloc", "realloc", "free", "aligned_alloc",
                                             "posix_memalign"};
    if (alloc.contains(mangled) || startsWith(name, "operator new") || startsWith(name, "operator delete"))
        return "calls the allocator (" + name + ")";
    if (mangled.starts_with("pthread_mutex_") || mangled.starts_with("pthread_cond_") ||
        mangled.starts_with("pthread_rwlock_") || mangled.starts_with("sem_") ||
        mangled == "pthread_join" || mangled == "syscall" || mangled.contains("futex"))
        return "waits in the threading library (" + name + ")";
    if (mangled == "printf" || mangled == "fprintf" || mangled == "puts" || mangled == "fputs" ||
        mangled == "write" || mangled == "read" || mangled == "fwrite" || mangled == "fflush" ||
        startsWith(name, "std::basic_ostream") || startsWith(name, "std::operator<<"))
        return "does input or output (" + name + ")";
    if (mangled == "sleep" || mangled == "usleep" || mangled == "nanosleep" || mangled == "sched_yield")
        return "sleeps or yields (" + name + ")";
    if (mangled.starts_with("__cxa_throw") || mangled.starts_with("__cxa_allocate_exception") ||
        mangled.starts_with("__cxa_guard_acquire"))
        return "throws or takes a static-initialisation lock (" + name + ")";
    return "calls " + name + ", whose body is not visible here, so it is counted as blocking";
}

class ProgressChecker {
public:
    explicit ProgressChecker(ModuleOp module) : module(module) { collectExemptions(); }

    std::optional<Finding> blockingIn(cir::FuncOp f);

private:
    ModuleOp module;
    llvm::DenseMap<Operation *, std::optional<Finding>> memo;
    llvm::DenseSet<Operation *> inProgress;
    // Set when a walk met a function still being walked further up: its
    // findings are not in yet, so a "nothing blocks" answer reached under
    // it is only provisional and must not be remembered.
    bool metCycle = false;
    // Stack slots holding the shared_ptr of a borrowed pop of an inline
    // queue: the destructor run on them frees nothing.
    llvm::DenseSet<Value> ownerlessSlots;

    void collectExemptions();
    bool ownerless(Value v) const;
    std::optional<Finding> blockingAt(Operation *op);
};

void ProgressChecker::collectExemptions() {
    module.walk([&](QueuePopOp pop) {
        if (pop.getBorrowed() && pop.getImpl() == Impl::spsc_inline && pop.getOut())
            ownerlessSlots.insert(pop.getOut());
    });
}

bool ProgressChecker::ownerless(Value v) const {
    while (auto up = v.getDefiningOp<cir::BaseClassAddrOp>()) v = up.getDerivedAddr();
    return ownerlessSlots.contains(v);
}

std::optional<Finding> ProgressChecker::blockingAt(Operation *op) {
    if (auto p = dyn_cast<ProgressOpInterface>(op)) {
        if (p.getProgress() == Progress::blocking || p.mayCallAllocator())
            return Finding{op, p.getBlockingReason(), {}};
        return std::nullopt;
    }
    if (op->getName().getStringRef().contains("asm"))
        return Finding{op, "runs inline assembly, which is not looked into", {}};
    auto call = dyn_cast<cir::CallOp>(op);
    if (!call) return std::nullopt;

    llvm::SmallVector<cir::FuncOp, 4> targets;
    if (cir::FuncOp callee = lookupCallee(call)) {
        // The shared_ptr of a borrowed pop owns nothing: its members free
        // nothing either.
        if (call.getNumOperands() > 0 && ownerless(call.getOperand(0))) return std::nullopt;
        targets.push_back(callee);
    } else {
        targets = indirectCallees(call);
        if (targets.empty())
            return Finding{op, "makes an indirect call whose target is not known here", {}};
    }
    for (cir::FuncOp t : targets) {
        if (t.getBody().empty()) {
            if (harmlessExternal(t.getSymName())) continue;
            return Finding{op, externalReason(t.getSymName()), {}};
        }
        if (auto f = blockingIn(t)) {
            Finding out = *f;
            out.chain.insert(out.chain.begin(), op);
            return out;
        }
    }
    return std::nullopt;
}

std::optional<Finding> ProgressChecker::blockingIn(cir::FuncOp f) {
    if (auto it = memo.find(f); it != memo.end()) return it->second;
    if (!inProgress.insert(f).second) {
        metCycle = true;  // its own walk, further up, reports what it finds
        return std::nullopt;
    }
    const bool outer = metCycle;
    metCycle = false;
    std::optional<Finding> found;
    f.walk([&](Operation *op) {
        if ((found = blockingAt(op))) return WalkResult::interrupt();
        return WalkResult::advance();
    });
    inProgress.erase(f);
    if (found || !metCycle) memo[f] = found;
    metCycle = outer || metCycle;
    return found;
}

class WTCCheckProgressPass : public PassWrapper<WTCCheckProgressPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCCheckProgressPass)

    StringRef getArgument() const final { return "wtc-check-progress"; }
    StringRef getDescription() const final {
        return "Fail the build when a function declared lock-free can reach a blocking operation "
               "(run after --wtc-specialize)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        ModuleOp module = getOperation();
        ProgressChecker checker(module);
        bool failed = false;
        module.walk([&](cir::FuncOp f) {
            if (!hasAnnotation(f, "wtc_progress_lock_free") || f.getBody().empty()) return;
            const std::string name = llvm::demangle(f.getSymName().str());
            auto found = checker.blockingIn(f);
            if (!found) {
                f.emitRemark() << "wtc: " << name << " is declared lock-free: nothing it reaches blocks";
                return;
            }
            failed = true;
            InFlightDiagnostic d = f.emitError() << "wtc: " << name << " is declared lock-free but can block";
            for (Operation *call : found->chain) {
                std::string callee = "an indirect callee";
                if (auto c = dyn_cast<cir::CallOp>(call))
                    if (cir::FuncOp t = lookupCallee(c)) callee = llvm::demangle(t.getSymName().str());
                d.attachNote(call->getLoc()) << "calls " << callee;
            }
            d.attachNote(found->leaf->getLoc()) << "which " << found->why;
        });
        if (failed) signalPassFailure();
        markAllAnalysesPreserved();
    }
};

} // namespace

namespace mlir::wtc {
void registerWTCCheckProgressPass() { PassRegistration<WTCCheckProgressPass>(); }
} // namespace mlir::wtc
