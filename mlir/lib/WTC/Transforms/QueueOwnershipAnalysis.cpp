#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/ThreadInstances.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallSet.h"

// For every queue object the module touches, how many threads push to it
// and how many pop from it, as a verdict written into the IR:
//
//   #wtc.topology<spsc>   one producer thread, one consumer thread
//   #wtc.topology<mpsc>   ... and so on
//   #wtc.topology<unknown> the analysis could not be sure
//
// The rule that makes the verdict usable for substitution is that every
// unresolved case maps to unknown, never to a smaller count. Substituting a
// single-producer structure into a program with two producers does not run
// slower, it races; so the analysis over-approximates threads and never
// under-approximates them.
//
// The walk over dynamic thread and function instances, which is the subtle
// part and is shared with every other analysis here, is in ThreadInstances.

namespace {

struct SideInfo {
    llvm::SmallSetVector<unsigned, 4> threads;
    bool anyMany = false;
    // 0 if nobody touches this side, 2 for "many", else 1.
    unsigned count() const {
        if (threads.empty()) return 0;
        return (anyMany || threads.size() >= 2) ? 2 : 1;
    }
};

struct RootInfo {
    SideInfo producers, consumers;
};

class QueueOwnershipAnalysisPass
    : public PassWrapper<QueueOwnershipAnalysisPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(QueueOwnershipAnalysisPass)

    StringRef getArgument() const final { return "queue-ownership-analysis"; }
    StringRef getDescription() const final {
        return "Count the threads that push to and pop from each wtc queue and write the "
               "verdict into the IR as #wtc.topology; unresolved cases yield unknown";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override;

private:
    ModuleOp module;
    llvm::MapVector<Operation *, RootInfo> roots;  // GlobalOp or AllocaOp, in discovery order
    llvm::DenseSet<Type> poisonedTypes;            // queue object types with an unresolvable access
    llvm::DenseMap<Operation *, llvm::SmallSetVector<Operation *, 2>> opRoots;
    bool globalPoison = false;   // an access with no type to poison
    unsigned remarks = 0;

    static bool isQueueOp(Operation *op) { return isa<QueuePushOp, QueuePopOp>(op); }

    static Type queueObjectType(Operation *root) {
        if (auto g = dyn_cast<cir::GlobalOp>(root)) return g.getSymType();
        if (auto a = dyn_cast<cir::AllocaOp>(root)) return a.getAllocaType();
        return Type();
    }

    static Type queueObjectTypeOfOp(Operation *op) {
        Value q = isa<QueuePushOp>(op) ? cast<QueuePushOp>(op).getQueue() : cast<QueuePopOp>(op).getQueue();
        if (auto qt = dyn_cast<QueueType>(q.getType())) return qt.getElementType();
        return Type();
    }

    void poison(Operation *op, const Twine &why) {
        Type t = queueObjectTypeOfOp(op);
        if (t) poisonedTypes.insert(t);
        else globalPoison = true;
        op->emitRemark() << "wtc: queue access is unresolvable (" << why
                         << "); every queue of this type is now unknown";
        ++remarks;
    }

    void poisonReachable(cir::FuncOp f) {
        llvm::DenseSet<Operation *> seen;
        llvm::SmallVector<cir::FuncOp, 8> work{f};
        while (!work.empty()) {
            cir::FuncOp cur = work.pop_back_val();
            if (!seen.insert(cur.getOperation()).second) continue;
            cur.walk([&](Operation *op) {
                if (isQueueOp(op)) poison(op, "reached through recursion");
                if (auto call = dyn_cast<cir::CallOp>(op))
                    if (auto callee = lookupCallee(call))
                        if (!callee.getBody().empty()) work.push_back(callee);
            });
        }
    }

    // A root's number is its position in discovery order, which follows
    // the walk and is therefore deterministic.
    int64_t idOf(Operation *root) const {
        return static_cast<int64_t>(roots.find(root) - roots.begin());
    }

    void writeVerdicts(bool capExceeded);
};

void QueueOwnershipAnalysisPass::writeVerdicts(bool capExceeded) {
    MLIRContext *ctx = &getContext();
    llvm::DenseMap<Operation *, Topology> verdictOf;

    for (auto &[root, info] : roots) {
        Topology t;
        const unsigned p = info.producers.count(), c = info.consumers.count();
        const Type objTy = queueObjectType(root);
        if (globalPoison || capExceeded || (objTy && poisonedTypes.count(objTy)))
            t = Topology::unknown;
        else if (p <= 1 && c <= 1)
            t = Topology::spsc;   // one side empty is still safe for an SPSC ring
        else if (p >= 2 && c <= 1)
            t = Topology::mpsc;
        else if (p <= 1 && c >= 2)
            t = Topology::spmc;
        else
            t = Topology::mpmc;
        verdictOf[root] = t;
        root->setAttr(kTopologyAttrName, TopologyAttr::get(ctx, t));
        root->setAttr(kQueueIdAttrName, IntegerAttr::get(IntegerType::get(ctx, 64), idOf(root)));

        std::string label;
        if (auto g = dyn_cast<cir::GlobalOp>(root)) label = ("@" + g.getSymName()).str();
        else if (auto a = dyn_cast<cir::AllocaOp>(root)) label = ("%" + a.getName()).str();
        else label = "<queue>";
        root->emitRemark() << "wtc: queue " << label << ": " << stringifyTopology(t)
                           << " (producers: " << (p == 2 ? "many" : std::to_string(p))
                           << ", consumers: " << (c == 2 ? "many" : std::to_string(c)) << ")";
        ++remarks;
    }

    // Each operation gets the meet over every root it may refer to.
    for (auto &[op, rs] : opRoots) {
        Topology t = Topology::unknown;
        bool first = true;
        for (Operation *r : rs) {
            Topology rt = verdictOf.lookup(r);
            if (first) { t = rt; first = false; }
            else if (rt != t) { t = Topology::unknown; }
        }
        if (auto push = dyn_cast<QueuePushOp>(op)) push.setTopologyAttr(TopologyAttr::get(ctx, t));
        else cast<QueuePopOp>(op).setTopologyAttr(TopologyAttr::get(ctx, t));
        // An operation that may act on several queues names none of them:
        // a later pass reading queue_id would otherwise treat the others'
        // operations as unrelated to this one.
        if (rs.size() != 1) continue;
        auto id = IntegerAttr::get(IntegerType::get(ctx, 64), idOf(rs.front()));
        if (auto push = dyn_cast<QueuePushOp>(op)) push.setQueueIdAttr(id);
        else cast<QueuePopOp>(op).setQueueIdAttr(id);
    }
    // Poisoned operations that resolved to nothing: unknown, explicitly.
    module.walk([&](Operation *op) {
        if (auto push = dyn_cast<QueuePushOp>(op)) { if (!push.getTopologyAttr()) push.setTopologyAttr(TopologyAttr::get(ctx, Topology::unknown)); }
        else if (auto pop = dyn_cast<QueuePopOp>(op)) { if (!pop.getTopologyAttr()) pop.setTopologyAttr(TopologyAttr::get(ctx, Topology::unknown)); }
    });
}

void QueueOwnershipAnalysisPass::runOnOperation() {
    module = getOperation();
    roots.clear(); poisonedTypes.clear(); opRoots.clear();
    globalPoison = false; remarks = 0;

    // A function that cannot reach a queue operation cannot affect a verdict.
    // The predicate needs the walk that owns the memo table, and the walk
    // needs the predicate, so the lambda is named: a temporary would die at
    // the end of the constructor call and leave function_ref dangling.
    ThreadInstanceWalk *self = nullptr;
    auto relevant = [&](cir::FuncOp f) { return self->reachesOp(f, isQueueOp); };
    ThreadInstanceWalk walk(module, relevant);
    self = &walk;

    walk.run(
        [&](Instance &inst) {
            inst.func.walk([&](Operation *op) {
                if (!isQueueOp(op)) return;
                Value q = isa<QueuePushOp>(op) ? cast<QueuePushOp>(op).getQueue()
                                               : cast<QueuePopOp>(op).getQueue();
                Operation *root = walk.resolveRoot(q, &inst);
                // The walk now resolves a heap pointer to the call that
                // allocated it. This pass has no notion of an allocation
                // that runs once against one that runs many times, and a
                // queue proved SPSC on the strength of one site that hands
                // out many objects would be wrong; until it reasons about
                // that, a heap queue stays unknown, as before.
                if (root && isa<cir::CallOp>(root)) root = nullptr;
                if (!root) { poison(op, "the queue object could not be identified"); return; }
                RootInfo &info = roots[root];
                SideInfo &side = isa<QueuePushOp>(op) ? info.producers : info.consumers;
                side.threads.insert(inst.thread);
                if (inst.threadMult == Mult::Many) side.anyMany = true;
                opRoots[op].insert(root);
            });
        },
        [&](cir::FuncOp f) { poisonReachable(f); });

    remarks += walk.remarks();

    // Two different failures, and telling them apart is the difference
    // between "this program is beyond me" and "this program is too big",
    // which call for different work. One message hid that.
    if (walk.capExceeded()) {
        module.emitRemark() << "wtc: gave up after " << ThreadInstanceWalk::kInstanceCap
                            << " function instances; every queue is unknown";
        ++remarks;
    }
    if (globalPoison) {
        module.emitRemark() << "wtc: an unresolvable queue access had no type to attribute it to; "
                               "every queue is unknown";
        ++remarks;
    }
    writeVerdicts(walk.capExceeded());
    markAllAnalysesPreserved();
}

} // namespace

namespace mlir::wtc {
    void registerQueueOwnershipAnalysisPass() {
        PassRegistration<QueueOwnershipAnalysisPass>();
    }
}
