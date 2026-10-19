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
// topology tag from generic (0) to spsc (1). Nothing else in the program
// changes: the shim's constructor selects the implementation from the tag,
// and every later operation on that object follows it.
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
        return "Rewrite the constructor tag of every queue the analysis proved spsc "
               "(run after --queue-ownership-analysis)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    Option<bool> rewrite{
        *this, "rewrite", llvm::cl::init(true),
        llvm::cl::desc("Rewrite the tags of queues proved spsc. With false the pass only records, "
                       "on each operation, the implementation its queue runs as the program "
                       "stands: the build of an unmodified source.")};

    ListOption<std::string> forceSpsc{
        *this, "force-spsc",
        llvm::cl::desc("Global queue symbols to tag spsc regardless of the verdict. "
                       "For the negative control only; this is how to build a program that races.")};

    void runOnOperation() override {
        ModuleOp module = getOperation();
        MLIRContext *ctx = &getContext();

        // Every constructor call, keyed by the root object it constructs.
        llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>> ctorsOf;
        module.walk([&](cir::CallOp call) {
            if (getSemanticKind(lookupCallee(call)) != SemanticOpKind::QueueConstruction) return;
            if (call.getNumOperands() == 0 || firstRealOperand(call) != 0) return;
            Value t = traceToRoot(call.getOperand(0));
            Operation *root = nullptr;
            if (auto gg = t.getDefiningOp<cir::GetGlobalOp>())
                root = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr()).getOperation();
            else if (auto a = t.getDefiningOp<cir::AllocaOp>())
                root = a.getOperation();
            if (root) ctorsOf[root].push_back(call);
        });

        unsigned rewritten = 0;
        module.walk([&](Operation *root) {
            if (!rewrite) return;
            if (!isa<cir::GlobalOp, cir::AllocaOp>(root)) return;
            bool wanted = false;
            if (auto t = root->getAttrOfType<TopologyAttr>(kTopologyAttrName))
                wanted = t.getValue() == Topology::spsc;
            if (auto g = dyn_cast<cir::GlobalOp>(root))
                for (const std::string &s : forceSpsc)
                    if (g.getSymName() == s) wanted = true;
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
            // A fresh constant rather than a mutation of the existing one: at
            // -O0 each use has its own, but nothing guarantees that.
            OpBuilder b(ctor);
            auto spsc = cir::ConstantOp::create(b, ctor.getLoc(), tagConst.getType(),
                                                cir::IntAttr::get(tagConst.getType(), 1));
            ctor->setOperand(1, spsc.getResult());
            root->setAttr(kSpecializedAttrName, TopologyAttr::get(ctx, Topology::spsc));
            ctor.emitRemark() << "wtc: specialised: constructor tag rewritten to spsc";
            ++rewritten;
        });
        (void)rewritten;
        recordImplementations(module, ctorsOf);
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
            cir::CallOp ctor = ctors.size() == 1 ? ctors.front() : cir::CallOp();
            if (ctor && ctor.getNumOperands() == 2)
                if (auto c = ctor.getOperand(1).getDefiningOp<cir::ConstantOp>())
                    if (auto v = dyn_cast<cir::IntAttr>(c.getValue()); v && v.getSInt() >= 0)
                        if (auto known = symbolizeImpl(static_cast<uint32_t>(v.getSInt())))
                            impl = *known;
            implOf[id.getInt()] = impl;
        }
        auto implFor = [&](IntegerAttr id) {
            if (!id) return Impl::generic;
            auto it = implOf.find(id.getInt());
            return it == implOf.end() ? Impl::generic : it->second;
        };
        module.walk([&](Operation *op) {
            if (auto push = dyn_cast<QueuePushOp>(op))
                push.setImplAttr(ImplAttr::get(ctx, implFor(push.getQueueIdAttr())));
            else if (auto pop = dyn_cast<QueuePopOp>(op))
                pop.setImplAttr(ImplAttr::get(ctx, implFor(pop.getQueueIdAttr())));
        });
    }
};

} // namespace

namespace mlir::wtc {
    void registerWTCSpecializePass() {
        PassRegistration<WTCSpecializePass>();
    }
}
