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
#include "llvm/Demangle/Demangle.h"

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
//
// A program can also state the topology itself, by passing a tag other than
// generic to the queue's constructor. That is a claim, and it is checked:
// when the threads that use the queue certainly break it -- two threads the
// module accounts for push to it, or one that certainly stands for many --
// the build fails, naming each thread and where it touches the queue; when
// the analysis cannot tell, a warning says why; when it agrees, a remark
// says the declaration is verified.

namespace {

// One access of a queue by one thread, kept for the declared-topology
// check, which has to name the threads it finds.
struct Access {
    Operation *op;
    unsigned thread;
    bool many;          // the thread certainly stands for several
    bool speculative;   // a thread the module cannot account for
    std::string entry;  // the function the thread starts in, demangled
    Location started;   // the spawn that starts it, or the entry for main
    bool spawned;
};

struct SideInfo {
    llvm::SmallSetVector<unsigned, 4> threads;
    llvm::SmallVector<Access, 4> accesses;
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
    llvm::DenseMap<Type, std::string> poisonWhy;   // the first reason, for the declared-topology check
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
        if (t && poisonedTypes.insert(t).second) poisonWhy[t] = why.str();
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
    // False when a declared topology is certainly broken.
    bool checkDeclared(Operation *root, const RootInfo &info, Topology verdict, const std::string &label,
                       llvm::ArrayRef<cir::CallOp> ctors, bool capExceeded);
    static Access accessOf(Operation *op, const Instance &inst);
};

// A location as a person reads it: file:line:column, from the first file
// position a fused or named location carries.
static std::string whereOf(Location loc) {
    if (auto f = dyn_cast<FileLineColLoc>(loc))
        return (f.getFilename().getValue() + ":" + Twine(f.getLine()) + ":" + Twine(f.getColumn())).str();
    if (auto f = dyn_cast<FusedLoc>(loc))
        for (Location inner : f.getLocations())
            if (std::string w = whereOf(inner); !w.empty()) return w;
    if (auto n = dyn_cast<NameLoc>(loc)) return whereOf(n.getChildLoc());
    if (auto c = dyn_cast<CallSiteLoc>(loc)) return whereOf(c.getCallee());
    return "";
}

Access QueueOwnershipAnalysisPass::accessOf(Operation *op, const Instance &inst) {
    // The instance that began this thread: up the chain while the thread
    // stays the same.
    const Instance *r = &inst;
    while (r->parent && r->parent->thread == r->thread) r = r->parent;
    const bool many = inst.threadMult == Mult::Many && !inst.execUnknown;
    cir::FuncOp entry = r->func;
    cir::CallOp site = r->site;
    return Access{op,
                  inst.thread,
                  many,
                  inst.speculative,
                  llvm::demangle(entry.getSymName().str()),
                  site ? site.getLoc() : entry.getLoc(),
                  static_cast<bool>(site)};
}

bool QueueOwnershipAnalysisPass::checkDeclared(Operation *root, const RootInfo &info, Topology verdict,
                                               const std::string &label,
                                               llvm::ArrayRef<cir::CallOp> ctors, bool capExceeded) {
    auto tag = constructorTag(ctors);
    if (!tag || *tag == 0) return true;  // nothing declared, or nothing readable
    cir::CallOp ctor = ctors.front();
    const char *declared = *tag == 2 ? "spsc_inline" : "spsc";
    if (verdict == Topology::spsc) {
        ctor.emitRemark() << "wtc: queue " << label << ": declared " << declared << ", verified";
        ++remarks;
        return true;
    }
    // Certainly broken: on one side, two threads the module accounts for,
    // or one that certainly stands for several.
    for (const SideInfo *side : {&info.producers, &info.consumers}) {
        const bool pushes = side == &info.producers;
        llvm::SmallVector<const Access *, 4> firstOfThread;
        llvm::SmallSet<unsigned, 4> seen;
        bool certainMany = false;
        for (const Access &a : side->accesses) {
            if (a.speculative) continue;
            if (a.many) certainMany = true;
            if (seen.insert(a.thread).second) firstOfThread.push_back(&a);
        }
        if (firstOfThread.size() < 2 && !certainMany) continue;
        InFlightDiagnostic d = ctor.emitError()
                               << "wtc: queue " << label << " is declared " << declared << " but ";
        if (certainMany) d << "many threads ";
        else d << firstOfThread.size() << " threads ";
        d << (pushes ? "push to it" : "pop from it");
        for (const Access *a : firstOfThread) {
            Diagnostic &n = d.attachNote(a->op->getLoc());
            n << (pushes ? "push" : "pop") << " on a thread that starts in " << a->entry;
            if (a->many) n << ", one of several started";
            if (a->spawned) {
                std::string w = whereOf(a->started);
                n << " at " << (w.empty() ? std::string("a spawn") : w);
            }
            else n << " (the program's own thread)";
        }
        return false;
    }
    // Not spsc, but nothing certain: say why it could not be checked.
    InFlightDiagnostic w = ctor.emitWarning()
                           << "wtc: queue " << label << " is declared " << declared << "; cannot verify: ";
    const Type objTy = queueObjectType(root);
    if (capExceeded) w << "the program has too many function instances";
    else if (globalPoison) w << "an access to some queue could not be attributed";
    else if (auto it = poisonWhy.find(objTy); it != poisonWhy.end()) w << it->second;
    else w << "a thread that uses it is one the module cannot account for (its caller is not visible)";
    return true;
}

void QueueOwnershipAnalysisPass::writeVerdicts(bool capExceeded) {
    MLIRContext *ctx = &getContext();
    llvm::DenseMap<Operation *, Topology> verdictOf;
    auto ctorsOf = collectQueueConstructors(module);
    bool contractsHold = true;

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
        if (auto it = ctorsOf.find(root); it != ctorsOf.end())
            contractsHold &= checkDeclared(root, info, t, label, it->second, capExceeded);
    }
    if (!contractsHold) signalPassFailure();

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
    roots.clear(); poisonedTypes.clear(); poisonWhy.clear(); opRoots.clear();
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
                side.accesses.push_back(accessOf(op, inst));
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
