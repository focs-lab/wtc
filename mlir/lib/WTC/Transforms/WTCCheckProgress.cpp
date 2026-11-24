#include "WTC/Transforms/Blocking.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/ProgressPaths.h"
#include "WTC/Transforms/ProgressRequirements.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/ProgressTable.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace mlir;
using namespace mlir::wtc;

// Holds a function declared lock-free (WTC_LOCK_FREE, shim/progress.hpp)
// to its promise: nothing it can reach may block. A function declared
// wait-free (WTC_WAIT_FREE, WTC_WAIT_FREE_WITHIN(k)) or progress-critical
// (WTC_PROGRESS_CRITICAL) is held to that and to what its composed paths
// must then be (ProgressRequirements.h). That is a reachability
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

// A bound, in words: its value when it has one, with the polynomial it
// came from where that says more.
std::string describeBound(const Bound &b) {
    if (!b.isPoly()) return b.str();
    if (auto v = b.instantiate(ThreadCounts::unknown())) {
        std::string s = "at most " + std::to_string(*v) + " steps";
        const std::string poly = b.str();
        if (poly != std::to_string(*v)) s += " (" + poly + ")";
        return s;
    }
    return "at most " + b.str() + " steps";
}

// One chain for each blocking site, as the error's notes.
void reportSites(InFlightDiagnostic &d, cir::FuncOp f, llvm::MutableArrayRef<BlockingSite> sites) {
    // Enough chains to act on; the rest are counted, not listed.
    constexpr size_t kMaxSites = 8;
    for (BlockingSite &found : sites.take_front(kMaxSites)) {
        for (auto [call, target] : llvm::zip(found.chain, found.via)) {
            const std::string callee = llvm::demangle(target.getSymName().str());
            auto c = dyn_cast<cir::CallOp>(call);
            if (!c) {
                d.attachNote(call->getLoc()) << "runs the element's " << describeSpecialMember(target) << ", "
                                             << callee;
                continue;
            }
            d.attachNote(call->getLoc()) << "calls " << callee << (!lookupCallee(c) ? " through a pointer" : "");
        }
        d.attachNote(found.leaf->getLoc()) << "which " << found.why;
    }
    if (const size_t more = sites.size() > kMaxSites ? sites.size() - kMaxSites : 0)
        d.attachNote(f.getLoc()) << "and " << more << " more blocking site" << (more == 1 ? "" : "s")
                                 << ", not shown";
}

// What a path guarantees, in words, for a remark.
std::string describePath(const PathProgress &p) {
    std::string s;
    if (p.partial) s += "partial (it waits for data), ";
    s += describeLevel(p.level).str();
    if (p.level == Level::wait_free) {
        s += ", " + describeBound(p.bound);
        if (p.amortised.str() != p.bound.str()) s += "; amortised " + describeBound(p.amortised);
    }
    if (p.alloc.kind != Alloc::Kind::never) s += "; allocates (" + p.alloc.str() + ")";
    if (p.kernel != Kernel::none) s += "; may enter the kernel (" + stringifyKernel(p.kernel).str() + ")";
    if (p.firstUse) s += "; on first use, a " + describeFirstUse(p.firstUse);
    if (!p.counts.empty()) s += "; with " + llvm::join(p.counts, ", ");
    return s;
}

class WTCCheckProgressPass : public PassWrapper<WTCCheckProgressPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCCheckProgressPass)

    StringRef getArgument() const final { return "wtc-check-progress"; }
    StringRef getDescription() const final {
        return "Fail the build when a function declared lock-free, wait-free or progress-critical can reach a "
               "blocking operation or its paths do not meet the promise (run after --wtc-specialize)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    Option<bool> inferredErrors{
        *this, "inferred-errors", llvm::cl::init(false),
        llvm::cl::desc("Fail the build, rather than warn, when a signal handler or a real-time thread misses the "
                       "requirement the program states without saying so (ProgressRequirements.h)")};

    WTCCheckProgressPass() = default;
    WTCCheckProgressPass(const WTCCheckProgressPass &other) : PassWrapper(other) {}

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
        PathComposer paths(module);
        RequirementRoots requirements(module);
        auto ctorsOf = collectQueueConstructors(module);
        llvm::DenseMap<int64_t, Operation *> rootOfId;
        module.walk([&](Operation *op) {
            if (auto id = op->getAttrOfType<IntegerAttr>(kQueueIdAttrName))
                if (op->hasAttr(kTopologyAttrName)) rootOfId.try_emplace(id.getInt(), op);
        });
        // A waiting pop's wait policy, from its queue's constructor: the
        // shim's queue takes it after the tag, and without it spins; a
        // policy that is not a constant may be spin.
        auto spins = [&](Operation *op) {
            auto id = op->getAttrOfType<IntegerAttr>("queue_id");
            Operation *root = id ? rootOfId.lookup(id.getInt()) : nullptr;
            if (!root) return true;
            llvm::SmallVector<cir::CallOp, 2> ctors;
            if (auto c = dyn_cast<cir::CallOp>(root)) ctors.push_back(c);
            else if (auto it = ctorsOf.find(root); it != ctorsOf.end()) ctors.assign(it->second.begin(), it->second.end());
            if (ctors.size() != 1) return true;
            cir::CallOp ctor = ctors.front();
            cir::FuncOp callee = lookupCallee(ctor);
            const std::string name = callee ? llvm::demangle(callee.getSymName().str()) : std::string();
            const llvm::StringRef n(name);
            // Where each constructor takes its policy: wtc::queue after
            // its tag (and spins without one), wtc::bounded fourth; the
            // lifted queues are built to spin, then park (shim/lifted.hpp).
            // Any other constructor's is not known.
            if (n.starts_with("wtc::lifted<") || n.starts_with("wtc::lifted_bounded<")) return false;
            unsigned at = 0;
            if (n.starts_with("wtc::queue<")) {
                if (ctor.getNumOperands() == 2) return true;
                at = 2;
            } else if (n.starts_with("wtc::bounded<")) {
                at = 4;
            } else {
                return true;
            }
            if (ctor.getNumOperands() <= at) return true;
            auto k = ctor.getOperand(at).getDefiningOp<cir::ConstantOp>();
            auto v = k ? dyn_cast<cir::IntAttr>(k.getValue()) : cir::IntAttr();
            return !v || v.getSInt() == 0;
        };
        // A lifted wait's waiter, by the condition variable it lives in: a
        // field by its record and index, a global by name. Its policy may
        // spin, then yield, before it parks (shim/lifted.hpp,
        // lifted_wait_policy), unless every construct of that waiter passes
        // paced as a constant true, which the wait lifting does for a wait
        // a requirement reaches (LiftWaits.cpp).
        auto waiterKey = [](Value at) -> std::optional<std::string> {
            Value r = traceToRoot(at);
            if (auto gg = r.getDefiningOp<cir::GetGlobalOp>()) return "global " + gg.getName().str();
            if (auto m = r.getDefiningOp<cir::GetMemberOp>()) {
                std::string ty;
                llvm::raw_string_ostream os(ty);
                os << m.getAddr().getType();
                return "member " + std::to_string(m.getIndex()) + " of " + ty;
            }
            return std::nullopt;
        };
        llvm::StringMap<bool> pacedWaiter;  // key -> every construct paced
        // A construct of a waiter that cannot be named and is not paced may
        // build any waiter: then none is known to park.
        bool unnamedUnpaced = false;
        module.walk([&](cir::CallOp c) {
            if (!hasAnnotation(lookupCallee(c), "wtc_lifted_wait_construct")) return;
            cir::BoolAttr b;
            if (c.getNumOperands() >= 4) {
                auto k = c.getOperand(3).getDefiningOp<cir::ConstantOp>();
                b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
            }
            const bool paced = b && b.getValue();
            auto key = c.getNumOperands() >= 1 ? waiterKey(c.getOperand(0)) : std::nullopt;
            if (!key) {
                unnamedUnpaced |= !paced;
                return;
            }
            auto [it, fresh] = pacedWaiter.try_emplace(*key, true);
            it->second = it->second && paced;
        });
        auto liftedStep = [](Operation *op) {
            cir::CallOp step;
            op->walk([&](cir::CallOp c) {
                if (hasAnnotation(lookupCallee(c), "wtc_lifted_wait_step")) step = c;
            });
            return step;
        };
        auto liftedParks = [&](cir::CallOp step) {
            if (unnamedUnpaced || step.getNumOperands() < 1) return false;
            auto key = waiterKey(step.getOperand(0));
            if (!key) return false;
            auto it = pacedWaiter.find(*key);
            return it != pacedWaiter.end() && it->second;
        };
        // On a real-time thread a wait for data that parks is allowed, and
        // so is a lifted wait whose waiter parks after its pause; nothing
        // under either is hidden by it, since the walk still enters what it
        // contains.
        auto parks = [&](Operation *op) {
            if (auto p = dyn_cast<ProgressOpInterface>(op)) {
                const ProgressSpec spec = p.getProgressSpec(ThreadCounts::unknown());
                return spec.partial && spec.level >= Level::wait_free && !spec.allocates() && !spec.firstUse &&
                       !spins(op);
            }
            if (!isa<cir::LoopOpInterface>(op)) return false;
            cir::CallOp step = liftedStep(op);
            return step && liftedParks(step);
        };
        // On a real-time thread a lifted wait's step whose waiter may yield
        // is refused where it is called, whatever the loop around it.
        auto rtSite = [&](Operation *op, bool inCallee) -> std::optional<BlockingSite> {
            cir::CallOp c = dyn_cast<cir::CallOp>(op);
            if (!c && isa<cir::LoopOpInterface>(op)) c = liftedStep(op);
            if (c)
                if (hasAnnotation(lookupCallee(c), "wtc_lifted_wait_step") && !liftedParks(c))
                    return BlockingSite{op, BlockingKind::Refused,
                                        "waits in a lifted wait whose policy may spin, then yield, before it parks: "
                                        "its waiter is not built paced everywhere (wtc::lifted_wait::construct), and "
                                        "a real-time thread never yields to a producer of lower priority on its "
                                        "processor",
                                        cir::FuncOp(), false, {}, {}};
            return loopSite(op, inCallee);
        };
        BlockingQuery rtBlocking(module, {BlockingKind::MayThrow}, rtSite, parks);
        for (const auto &[at, why] : requirements.unresolved())
            emitWarning(at->getLoc()) << "wtc: " << why << ", so what it runs is not checked";
        for (const RequirementRoot &root : requirements.roots()) {
            cir::FuncOp f = root.func;  // a copy: the handle is not const
            const Requirement *req = &root.req;
            const std::string name = llvm::demangle(f.getSymName().str());
            // An inferred requirement is the program's own, unstated: a
            // warning, unless the build asks for errors.
            const bool hard = !root.inferred() || inferredErrors;
            auto complain = [&](const llvm::Twine &text) -> InFlightDiagnostic {
                if (hard) failed = true;
                InFlightDiagnostic d = hard ? f.emitError() : f.emitWarning();
                d << text;
                return d;
            };
            const std::string declared =
                root.inferred() ? "is " + root.why + ", so it must be " +
                                      (root.source == RequirementRoot::Source::handler
                                           ? std::string("async-signal-safe")
                                           : std::string("wait-free but for waits for data"))
                                : "is declared " + req->describe();
            const bool realtime = root.source == RequirementRoot::Source::realtime;
            auto sites = realtime ? rtBlocking.sitesIn(f) : blocking.sitesIn(f);
            // On a real-time thread, why a wait for data is not allowed: it
            // spins, or it takes a lock on first use.
            if (realtime)
                for (BlockingSite &site : sites) {
                    auto p = dyn_cast<ProgressOpInterface>(site.leaf);
                    if (!p || (!site.chain.empty() && !isa<cir::CallOp>(site.chain.back()))) continue;
                    const ProgressSpec spec = p.getProgressSpec(ThreadCounts::unknown());
                    if (!spec.partial || spec.level < Level::wait_free || spec.allocates()) continue;
                    if (spins(site.leaf))
                        site.why = "waits for a push by spinning (its queue's wait policy is spin): a real-time thread "
                                   "never yields to a producer of lower priority on its processor";
                    else if (spec.firstUse & first_use_guard)
                        site.why = "takes, the first time it runs in the process, a static-initialisation lock";
                }
            for (BlockingSite &site : sites)
                if (root.source == RequirementRoot::Source::realtime && isa<cir::LoopOpInterface>(site.leaf) &&
                    llvm::StringRef(site.why).starts_with("waits for an element"))
                    site.why += "; it spins, and a real-time thread never yields to a producer of lower priority on "
                                "its processor";
            // What the function's paths guarantee, composed: the level, the
            // steps under the counts proved, and the effects beside them.
            const PathProgress path = paths.of(f);
            auto composed = [&]() {
                InFlightDiagnostic r = f.emitRemark() << "wtc: " << name << ": " << describePath(path);
                if (path.weakest) r.attachNote(path.weakest->getLoc()) << "the weakest step: " << path.weakWhy;
            };
            auto origin = [&](InFlightDiagnostic &d) {
                if (root.origin) d.attachNote(root.origin->getLoc()) << "where the requirement comes from";
            };
            if (!sites.empty()) {
                InFlightDiagnostic d =
                    complain("wtc: " + name + " " + declared + (root.inferred() ? ", but it can block" : " but can block"));
                origin(d);
                reportSites(d, f, sites);
                d.report();
                composed();
                continue;
            }
            // Nothing blocks. A stronger promise than lock-freedom asks the
            // composition the rest.
            std::string why;
            Operation *at = path.weakest;
            if (root.source == RequirementRoot::Source::handler && path.firstUse)
                why = "takes, on first use, a " + describeFirstUse(path.firstUse);
            if (why.empty() && req->kind >= Requirement::Kind::wait_free) {
                if (path.level < Level::wait_free)
                    why = std::string("is only ") + describeLevel(path.level).str();
                else if (path.partial && root.source != RequirementRoot::Source::realtime)
                    why = "waits for data";
                else if (req->kind == Requirement::Kind::critical && !path.bound.isPoly())
                    why = "has a worst case " + path.bound.str();
                else if (req->kind == Requirement::Kind::critical && path.firstUse)
                    why = "takes, on first use, a " + describeFirstUse(path.firstUse);
                else if (req->within) {
                    auto v = path.bound.instantiate(ThreadCounts::unknown());
                    if (!v)
                        why = "has a bound that is not a number here: " + path.bound.str() +
                              (path.counts.empty() ? "" : ", with " + llvm::join(path.counts, ", "));
                    else if (*v > *req->within)
                        why = "may take " + std::to_string(*v) + " steps (" + path.bound.str() + ")";
                }
            }
            if (!why.empty()) {
                InFlightDiagnostic d = root.inferred() ? complain("wtc: " + name + " " + declared + ", but it " + why)
                                                       : complain("wtc: " + name + " " + declared + " but " + why);
                origin(d);
                if (at) d.attachNote(at->getLoc()) << "the weakest step: " << path.weakWhy;
                d.report();
                composed();
                continue;
            }
            if (root.inferred())
                f.emitRemark() << "wtc: " << name << " " << declared << ": it is";
            else if (req->kind == Requirement::Kind::lock_free)
                f.emitRemark() << "wtc: " << name << " " << declared << ": nothing it reaches blocks";
            else
                f.emitRemark() << "wtc: " << name << " " << declared << ": it is";
            composed();
        }
        // A handler that pushes interrupts its own thread, which may be in
        // the middle of a push to the same queue: a cell taken and not yet
        // filled. Only a consumer that looks past such a cell makes progress
        // past it: the single-consumer queue's (mpsc, jiffy), and a ring of
        // indices', whose push publishes its index only once its cell is
        // filled (scq, wcq). Every other consumer waits for the push the
        // handler interrupted, which cannot finish until the handler
        // returns. A handler that then waits for the consumer (to flush a
        // log) never returns.
        module.walk([&](Operation *op) {
            if (!isa<QueuePushOp, QueuePushDeferredOp, QueueBatchEndOp>(op)) return;
            llvm::SmallVector<cir::CallOp, 4> chain;
            const RequirementRoot *root =
                requirements.reaching(op->getParentOfType<cir::FuncOp>(), &chain,
                                      [](const RequirementRoot &r) { return r.source == RequirementRoot::Source::handler; });
            if (!root) return;
            std::optional<Impl> impl;
            bool bounded = false;
            if (auto p = dyn_cast<QueuePushOp>(op)) impl = p.getImpl(), bounded = p.getBounded();
            else if (auto d = dyn_cast<QueuePushDeferredOp>(op)) impl = d.getImpl();
            else impl = cast<QueueBatchEndOp>(op).getImpl();
            const Impl i = impl.value_or(Impl::generic);
            if (!bounded && (i == Impl::mpsc || i == Impl::jiffy)) return;
            if (bounded && (i == Impl::scq || i == Impl::wcq)) return;
            InFlightDiagnostic d = inferredErrors ? op->emitError() : op->emitWarning();
            if (inferredErrors) failed = true;
            cir::FuncOp handler = root->func;
            d << "wtc: " << llvm::demangle(handler.getSymName().str()) << ", " << root->why
              << ", pushes to a queue whose consumer waits for a push in progress ("
              << (bounded ? "bounded" : stringifyImpl(i).str())
              << "): a push the handler interrupts on its own thread holds a cell the consumer waits for until the "
                 "handler returns; only the single-consumer queue (mpsc) and the rings of indices look past such a "
                 "cell";
            for (cir::CallOp c : chain)
                d.attachNote(c.getLoc()) << "calls " << llvm::demangle(lookupCallee(c) ? lookupCallee(c).getSymName().str() : std::string("through a pointer"));
            if (root->origin) d.attachNote(root->origin->getLoc()) << "where the handler is installed";
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
