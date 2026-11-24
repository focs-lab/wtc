#include "WTC/Transforms/Blocking.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/ProgressPaths.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/ProgressTable.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

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
// that cannot wait. A call to a function with a body is followed into it,
// and so is the element's special member a queue operation runs: a push
// runs its copy or move constructor, a pop its move-assignment and
// destructor, inside the operation.
// A call that may throw is not itself a reason: what it throws with is in
// the body, or is counted with it. A loop is judged by what its exit
// depends on (ProgressPaths.h): one that spins until another thread writes
// what it reads, or until a queue has an element, blocks; a retry on a
// compare-and-swap is lock-free and passes.
//
// One exception, and it is what makes a consumer of an inline queue pass:
// the destructor of the shared_ptr a pop returns, when the pop is borrowed
// and its queue holds elements inline, frees nothing -- the shared_ptr
// owns nothing -- and is not followed.

// A build that wants its binary whatever the promises say (a test that
// runs a program whose first-use effects fail them): the diagnostics stay,
// the pass does not fail.
static llvm::cl::opt<bool> clWarnOnly("wtc-progress-report-only",
                                      llvm::cl::desc("Report what --wtc-check-progress finds without failing"),
                                      llvm::cl::init(false));

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
        // A loop is judged by what its exit depends on (ProgressPaths.h):
        // one that waits for another thread, or for an element of a queue,
        // blocks; a retry, a counted loop and one over the thread's own
        // data do not.
        LoopClassifier loops(module);
        auto loopSite = [&](Operation *op, bool) -> std::optional<BlockingSite> {
            std::optional<LoopClass> c;
            if (isa<cir::LoopOpInterface>(op)) c = loops.classify(op);
            else if (auto go = dyn_cast<cir::GotoOp>(op)) c = loops.classifyGoto(go);
            if (!c || (c->kind != LoopKind::Wait && c->kind != LoopKind::Dual && c->kind != LoopKind::Forever))
                return std::nullopt;
            return BlockingSite{op, BlockingKind::Refused, c->why, cir::FuncOp(), false, {}, {}};
        };
        BlockingQuery blocking(module, {BlockingKind::MayThrow}, loopSite);
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
                    const std::string callee = llvm::demangle(target.getSymName().str());
                    auto c = dyn_cast<cir::CallOp>(call);
                    if (!c) {
                        d.attachNote(call->getLoc()) << "runs the element's " << describeSpecialMember(target)
                                                     << ", " << callee;
                        continue;
                    }
                    d.attachNote(call->getLoc()) << "calls " << callee << (!lookupCallee(c) ? " through a pointer" : "");
                }
                d.attachNote(found.leaf->getLoc()) << "which " << found.why;
            }
            if (const size_t more = sites.size() > kMaxSites ? sites.size() - kMaxSites : 0)
                d.attachNote(f.getLoc()) << "and " << more << " more blocking site"
                                         << (more == 1 ? "" : "s") << ", not shown";
        });
        if (failed && !clWarnOnly) signalPassFailure();
        markAllAnalysesPreserved();
    }
};

// Prints the progress table (WTC/ProgressTable.h), one row per form,
// implementation and counts, as a Markdown table: what docs/progress.md
// shows and a test checks, so that the prose is generated from the code.
class WTCProgressTablePass : public PassWrapper<WTCProgressTablePass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCProgressTablePass)

    StringRef getArgument() const final { return "wtc-progress-table"; }
    StringRef getDescription() const final {
        return "Print the progress of every wtc operation by form, implementation and thread counts";
    }

    // A bound in the counts, and its value where the row's counts are one.
    static std::string withValue(const Bound &b, const ThreadCounts &c) {
        std::string s = b.str();
        if (b.dependsOnCounts())
            if (auto v = b.instantiate(c)) s += " (= " + std::to_string(*v) + ")";
        return s;
    }

    void runOnOperation() override {
        llvm::raw_ostream &os = llvm::outs();
        os << "| operation | impl | np | nc | level | bound | amortised | allocates | kernel | first use | element | "
              "reason |\n";
        os << "|---|---|---|---|---|---|---|---|---|---|---|---|\n";
        for (unsigned f = 0; f <= static_cast<unsigned>(OpForm::delegate); ++f) {
            const auto form = static_cast<OpForm>(f);
            for (Impl impl : implsFor(form)) {
                auto counts = isQueueForm(form) ? countsFor(impl) : llvm::SmallVector<ThreadCounts, 4>{ThreadCounts{
                                                                        Count::unbounded(), Count::unbounded(),
                                                                        Count::unbounded()}};
                for (const ThreadCounts &c : counts) {
                    ProgressSpec s = progressSpecFor(form, impl, c);
                    auto side = [](const Count &n) { return n.isOne() ? std::string("1") : std::string("many"); };
                    os << "| " << stringifyOpForm(form) << " | " << (isQueueForm(form) ? stringifyImpl(impl) : "-")
                       << " | " << (isQueueForm(form) ? side(c.producers) : "-") << " | "
                       << (isQueueForm(form) ? side(c.consumers) : "-") << " | " << (s.partial ? "partial(" : "")
                       << stringifyLevel(s.level) << (s.partial ? ")" : "") << " | "
                       << (s.level == Level::wait_free ? withValue(s.bound, c) : "-") << " | "
                       << (s.amortised.kind == Bound::Kind::None ? "-" : withValue(s.amortised, c)) << " | "
                       << s.alloc.str()
                       << " | " << stringifyKernel(s.kernel) << " | " << describeFirstUse(s.firstUse) << " | "
                       << describeElementCode(s.element) << " | " << (s.reason.empty() ? "-" : s.reason) << " |\n";
                }
            }
        }
        markAllAnalysesPreserved();
    }
};

} // namespace

namespace mlir::wtc {
void registerWTCCheckProgressPass() {
    PassRegistration<WTCCheckProgressPass>();
    PassRegistration<WTCProgressTablePass>();
}
} // namespace mlir::wtc
