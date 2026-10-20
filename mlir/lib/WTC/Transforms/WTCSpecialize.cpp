#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

// The substitution. For each queue object the analysis marked spsc, find
// the one call that constructs it and rewrite the constant it passes as the
// topology tag from generic (0) to spsc (1), or to spsc_inline (2) when the
// borrow analysis proved that every pop only borrows its element and the
// element type may be held inline; for one marked mpsc, to mpsc (3), and
// for one marked spmc, to spmc (4). An mpmc queue, or one the analysis
// could not decide, keeps the general-purpose implementation. Nothing else
// in the program changes: the shim's constructor selects the
// implementation from the tag, and every later operation on that object
// follows it.
//
// The pattern, as ClangIR emits it for a defaulted enum argument:
//
//   %q   = cir.get_global @q1 : !cir.ptr<!rec_wtc..queue..>     (or an alloca)
//   %tag = cir.const #cir.int<0> : !s32i
//   cir.call @_ZN3wtc5queueI4ItemEC2ENS_8topologyE(%q, %tag)
//
// Anything that does not match exactly is left alone with a remark: a tag
// that is not a constant, a tag the program already set, no constructor
// call found for the object, or more than one. Leaving the generic
// implementation in place is always correct, so every doubt resolves that
// way.
//
// Afterwards every queue operation records, as `impl`, the implementation
// its queue runs, so that later passes need not find the constructor
// again.

namespace {

class WTCSpecializePass : public PassWrapper<WTCSpecializePass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCSpecializePass)

    WTCSpecializePass() = default;
    WTCSpecializePass(const WTCSpecializePass &other) : PassWrapper(other) {}

    StringRef getArgument() const final { return "wtc-specialize"; }
    StringRef getDescription() const final {
        return "Rewrite the constructor tag of every queue the analysis proved to have one "
               "producer or one consumer (run after --queue-ownership-analysis)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    Option<bool> rewrite{
        *this, "rewrite", llvm::cl::init(true),
        llvm::cl::desc("Rewrite the tags of queues proved spsc, mpsc or spmc. With false the pass only records, "
                       "on each operation, the implementation its queue runs as the program "
                       "stands: the build of an unmodified source.")};

    ListOption<std::string> forceInline{
        *this, "force-inline",
        llvm::cl::desc("Global queue symbols to tag spsc_inline regardless of the verdicts. "
                       "For the negative control only: a consumer that keeps what it pops then "
                       "reads an element that has been destroyed.")};

    ListOption<std::string> forceSpsc{
        *this, "force-spsc",
        llvm::cl::desc("Global queue symbols to tag spsc regardless of the verdict. "
                       "For the negative control only; this is how to build a program that races.")};

    void runOnOperation() override {
        ModuleOp module = getOperation();
        MLIRContext *ctx = &getContext();

        // Every constructor call, keyed by the root object it constructs. A
        // queue that is a member of a struct or a heap object has the one
        // call that builds it as its root.
        auto ctorsOf = collectQueueConstructors(module);
        module.walk([&](cir::CallOp call) {
            if (call->hasAttr(kTopologyAttrName)) ctorsOf[call.getOperation()] = {call};
        });

        unsigned rewritten = 0;
        module.walk([&](Operation *root) {
            if (!rewrite) return;
            if (!isa<cir::GlobalOp, cir::AllocaOp>(root) &&
                !(isa<cir::CallOp>(root) && root->hasAttr(kTopologyAttrName)))
                return;
            bool wanted = false, forcedInline = false, forced = false;
            Topology verdict = Topology::unknown;
            if (auto t = root->getAttrOfType<TopologyAttr>(kTopologyAttrName)) {
                verdict = t.getValue();
                wanted = verdict == Topology::spsc || verdict == Topology::mpsc ||
                         verdict == Topology::spmc;
            }
            if (auto g = dyn_cast<cir::GlobalOp>(root)) {
                for (const std::string &s : forceSpsc)
                    if (g.getSymName() == s) wanted = forced = true;
                for (const std::string &s : forceInline)
                    if (g.getSymName() == s) wanted = forcedInline = true;
            }
            if (!wanted) return;

            auto it = ctorsOf.find(root);
            if (it == ctorsOf.end() || it->second.empty()) {
                root->emitRemark() << "wtc: not specialised: no constructor call found for this queue";
                return;
            }
            if (it->second.size() != 1) {
                root->emitRemark() << "wtc: not specialised: " << it->second.size()
                                   << " constructor calls reach this queue";
                return;
            }
            cir::CallOp ctor = it->second.front();
            if (ctor.getNumOperands() != 2) {
                ctor.emitRemark() << "wtc: not specialised: the constructor does not take a topology tag";
                return;
            }
            auto tagConst = ctor.getOperand(1).getDefiningOp<cir::ConstantOp>();
            auto tagVal = tagConst ? dyn_cast<cir::IntAttr>(tagConst.getValue()) : cir::IntAttr();
            if (!tagVal) {
                ctor.emitRemark() << "wtc: not specialised: the topology tag is not a constant";
                return;
            }
            if (tagVal.getSInt() != 0) {
                ctor.emitRemark() << "wtc: not specialised: the program sets the tag itself ("
                                  << tagVal.getSInt() << "); left as declared";
                return;
            }
            // Inline storage needs both the borrow proof and an element type
            // the shim can hold inline, which its constructor says by calling
            // a marker; without either, plain spsc. Many producers and one
            // consumer is the list without locks, one producer and many
            // consumers the ring whose consumers take turns, whatever the
            // pops do.
            const bool manySide = (verdict == Topology::mpsc || verdict == Topology::spmc) &&
                                  !forced && !forcedInline;
            Impl to = !manySide ? Impl::spsc : verdict == Topology::mpsc ? Impl::mpsc : Impl::spmc;
            if (forcedInline) {
                to = Impl::spsc_inline;
            } else if (!manySide && root->hasAttr(kBorrowedAttrName)) {
                if (marksInlineOk(lookupCallee(ctor)))
                    to = Impl::spsc_inline;
                else
                    ctor.emitRemark() << "wtc: every pop borrows, but this element type cannot be "
                                         "held inline; plain spsc";
            }
            // A fresh constant rather than a mutation of the existing one: at
            // -O0 each use has its own, but nothing guarantees that.
            OpBuilder b(ctor);
            auto tag = cir::ConstantOp::create(
                b, ctor.getLoc(), tagConst.getType(),
                cir::IntAttr::get(tagConst.getType(), static_cast<int64_t>(to)));
            ctor->setOperand(1, tag.getResult());
            root->setAttr(kSpecializedAttrName, TopologyAttr::get(ctx, manySide ? verdict : Topology::spsc));
            ctor.emitRemark() << "wtc: specialised: constructor tag rewritten to "
                              << stringifyImpl(to);
            ++rewritten;
        });
        (void)rewritten;
        recordImplementations(module, ctorsOf);
    }

    // The shim's constructor calls a function annotated wtc_queue_inline_ok
    // exactly when its element type may be held inline.
    static bool marksInlineOk(cir::FuncOp ctor) {
        if (!ctor || ctor.getBody().empty()) return false;
        bool found = false;
        ctor.walk([&](cir::CallOp call) {
            if (hasAnnotation(lookupCallee(call), "wtc_queue_inline_ok")) found = true;
        });
        return found;
    }

    // What each numbered queue runs, read back from the tag its one
    // constructor passes once the rewriting above is done, and written on
    // every operation that can only act on that queue. Anything else -- no
    // constructor found, several, a tag that is not a constant, an
    // operation that may act on several queues -- is the general-purpose
    // implementation: the answer that is never wrong to assume.
    static void recordImplementations(
        ModuleOp module, const llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>> &ctorsOf) {
        MLIRContext *ctx = module.getContext();
        llvm::DenseMap<int64_t, Impl> implOf;
        for (auto &[root, ctors] : ctorsOf) {
            auto id = root->getAttrOfType<IntegerAttr>(kQueueIdAttrName);
            if (!id) continue;
            Impl impl = Impl::generic;
            if (auto tag = constructorTag(ctors); tag && *tag >= 0)
                if (auto known = symbolizeImpl(static_cast<uint32_t>(*tag))) impl = *known;
            implOf[id.getInt()] = impl;
        }
        auto implFor = [&](IntegerAttr id) {
            if (!id) return Impl::generic;
            auto it = implOf.find(id.getInt());
            return it == implOf.end() ? Impl::generic : it->second;
        };
        module.walk([&](Operation *op) {
            if (!isa<QueuePushOp, QueuePopOp, QueueBatchBeginOp, QueuePushDeferredOp, QueueBatchEndOp>(op))
                return;
            op->setAttr("impl", ImplAttr::get(ctx, implFor(op->getAttrOfType<IntegerAttr>("queue_id"))));
        });
    }
};

} // namespace

namespace mlir::wtc {
    void registerWTCSpecializePass() {
        PassRegistration<WTCSpecializePass>();
    }
}
