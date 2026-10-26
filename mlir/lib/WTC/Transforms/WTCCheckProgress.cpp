#include "WTC/Transforms/Blocking.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Demangle/Demangle.h"

#include <string>

using namespace mlir;
using namespace mlir::wtc;

// Holds a function declared lock-free (WTC_LOCK_FREE, shim/progress.hpp)
// to its promise: nothing it can reach may block. That is a reachability
// question over the call graph, and the answer is far more use as a call
// chain than as a latency spike in production.
//
// What blocks is the shared answer (Transforms/Blocking.h, decisions/0004),
// with no kind accepted: a wtc operation whose implementation can wait or
// may call the allocator, a call to a function without a body here that is
// known to block or cannot be seen into, an indirect call whose targets
// are not known, a throw, and inline assembly not made only of instructions
// that cannot wait. A call to a function with a body is followed into it.
// A call that may throw is not itself a reason: what it throws with is in
// the body, or is counted with it. A loop that spins on an atomic waiting
// for another thread is not judged: the check is a necessary condition for
// lock-freedom, not a proof of it.
//
// One exception, and it is what makes a consumer of an inline queue pass:
// the destructor of the shared_ptr a pop returns, when the pop is borrowed
// and its queue holds elements inline, frees nothing -- the shared_ptr
// owns nothing -- and is not followed.

namespace {

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
        BlockingQuery blocking(module, {BlockingKind::MayThrow});
        bool failed = false;
        module.walk([&](cir::FuncOp f) {
            if (!hasAnnotation(f, "wtc_progress_lock_free") || f.getBody().empty()) return;
            const std::string name = llvm::demangle(f.getSymName().str());
            auto sites = blocking.sitesIn(f);
            if (sites.empty()) {
                f.emitRemark() << "wtc: " << name << " is declared lock-free: nothing it reaches blocks";
                return;
            }
            failed = true;
            InFlightDiagnostic d = f.emitError() << "wtc: " << name << " is declared lock-free but can block";
            // Enough chains to act on; the rest are counted, not listed.
            constexpr size_t kMaxSites = 8;
            for (BlockingSite &found : llvm::MutableArrayRef(sites).take_front(kMaxSites)) {
                for (auto [call, target] : llvm::zip(found.chain, found.via)) {
                    const bool indirect = !lookupCallee(cast<cir::CallOp>(call));
                    d.attachNote(call->getLoc()) << "calls " << llvm::demangle(target.getSymName().str())
                                                 << (indirect ? " through a pointer" : "");
                }
                d.attachNote(found.leaf->getLoc()) << "which " << found.why;
            }
            if (const size_t more = sites.size() > kMaxSites ? sites.size() - kMaxSites : 0)
                d.attachNote(f.getLoc()) << "and " << more << " more blocking site"
                                         << (more == 1 ? "" : "s") << ", not shown";
        });
        if (failed) signalPassFailure();
        markAllAnalysesPreserved();
    }
};

} // namespace

namespace mlir::wtc {
void registerWTCCheckProgressPass() { PassRegistration<WTCCheckProgressPass>(); }
} // namespace mlir::wtc
