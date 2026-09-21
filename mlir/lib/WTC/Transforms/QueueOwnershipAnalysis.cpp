#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallSet.h"

#include <deque>

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
// Threads are dynamic, not static: a spawn site inside a loop, or in a
// function that runs more than once, stands for many threads. A function
// instance is a function under a particular chain of call sites from a
// root, and carries the call site that created it, so a parameter can be
// resolved to the argument that particular caller passed. That is what
// makes "which queue does this push touch" answerable for a function that
// takes the queue as a parameter, and what distinguishes two queues passed
// to the same function.
//
// Roots of the walk: main; the functions global constructors run (on the
// main thread, once); every function whose address is taken other than as a
// spawn entry (function pointers, std::thread used directly, virtual calls,
// std::function), which may be called from any thread any number of times;
// and any remaining externally visible function nobody in this translation
// unit calls. The whole-program assumption is one translation unit, stated
// here because nothing enforces it.

namespace {

enum class Mult { One, Many };

Mult join(Mult a, Mult b) { return (a == Mult::One && b == Mult::One) ? Mult::One : Mult::Many; }

struct Instance {
    cir::FuncOp func;
    unsigned thread;
    Mult threadMult;   // how many dynamic threads this instance's thread stands for
    Mult execMult;     // how many times the function runs within that thread
    cir::CallOp site;  // the call or spawn that created this instance; null for a root
    Instance *parent;
    bool unknownContext;
};

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
    static constexpr unsigned kInstanceCap = 10000;

    ModuleOp module;
    unsigned nextThread = 0;
    std::deque<Instance> instances;                // stable addresses
    llvm::MapVector<Operation *, RootInfo> roots;  // GlobalOp or AllocaOp, in discovery order
    llvm::DenseSet<Type> poisonedTypes;            // queue object types with an unresolvable access
    llvm::DenseMap<Operation *, llvm::SmallSetVector<Operation *, 2>> opRoots;
    llvm::DenseMap<Operation *, bool> unstructuredCache;
    llvm::DenseMap<Operation *, bool> reachesQueueCache;
    llvm::DenseSet<Operation *> reachedFuncs;
    bool globalPoison = false;   // an access with no type to poison
    bool capExceeded = false;    // too many instances to be sure of anything
    unsigned remarks = 0;

    Instance &addInstance(Instance inst) {
        reachedFuncs.insert(inst.func.getOperation());
        instances.push_back(inst);
        return instances.back();
    }

    // Whether this function, or anything it can reach by a direct call or by
    // spawning a thread, contains a queue operation. A function that reaches
    // none cannot affect any verdict, so it needs no instance and no remark.
    // Prunes the walk and keeps the diagnostics about the program rather
    // than about the shim's own destructors.
    bool reachesQueueOp(cir::FuncOp f) {
        auto it = reachesQueueCache.find(f.getOperation());
        if (it != reachesQueueCache.end()) return it->second;
        reachesQueueCache[f.getOperation()] = false;   // cycles answer "no" here
        bool found = false;
        f.walk([&](Operation *op) {
            if (isa<QueuePushOp, QueuePopOp>(op)) { found = true; return WalkResult::interrupt(); }
            auto call = dyn_cast<cir::CallOp>(op);
            if (!call) return WalkResult::advance();
            cir::FuncOp callee = lookupCallee(call);
            if (!callee) return WalkResult::advance();
            if (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn) {
                const unsigned k = firstRealOperand(call);
                if (k < call.getNumOperands())
                    if (cir::FuncOp entry = resolveFunctionOperand(call.getOperand(k)))
                        if (reachesQueueOp(entry)) { found = true; return WalkResult::interrupt(); }
                return WalkResult::advance();
            }
            if (!callee.getBody().empty() && reachesQueueOp(callee)) {
                found = true;
                return WalkResult::interrupt();
            }
            return WalkResult::advance();
        });
        reachesQueueCache[f.getOperation()] = found;
        return found;
    }

    bool isUnstructured(cir::FuncOp f) {
        auto it = unstructuredCache.find(f.getOperation());
        if (it != unstructuredCache.end()) return it->second;
        bool found = false;
        f.walk([&](Operation *op) {
            if (isa<cir::GotoOp, cir::LabelOp, cir::BrOp, cir::BrCondOp,
                    cir::IndirectBrOp, cir::SwitchFlatOp>(op))
                found = true;
        });
        unstructuredCache[f.getOperation()] = found;
        return found;
    }

    // Many if the operation sits inside a loop of its function, or the
    // function's control flow is not structured enough to tell.
    Mult siteMult(Operation *op, cir::FuncOp f) {
        if (isUnstructured(f)) return Mult::Many;
        for (Operation *p = op->getParentOp(); p && p != f.getOperation(); p = p->getParentOp())
            if (isa<cir::LoopOpInterface>(p)) return Mult::Many;
        return Mult::One;
    }

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
                if (isa<QueuePushOp, QueuePopOp>(op)) poison(op, "reached through recursion");
                if (auto call = dyn_cast<cir::CallOp>(op))
                    if (auto callee = lookupCallee(call))
                        if (!callee.getBody().empty()) work.push_back(callee);
            });
        }
    }

    // The root object a queue handle refers to, resolved through the chain
    // of call sites that created this instance, or null.
    Operation *resolveRoot(Value v, Instance *inst, unsigned depth = 0) {
        if (depth > 64 || !inst) return nullptr;
        Value t = traceToRoot(v);
        if (auto gg = t.getDefiningOp<cir::GetGlobalOp>())
            return SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr()).getOperation();
        if (auto alloca = t.getDefiningOp<cir::AllocaOp>())
            return alloca.getOperation();
        auto arg = dyn_cast<BlockArgument>(t);
        if (!arg) return nullptr;
        if (!inst->site) return nullptr;                       // a root: nobody passed it
        if (arg.getOwner() != &inst->func.getBody().front()) return nullptr;
        const unsigned i = arg.getArgNumber();
        cir::CallOp site = inst->site;
        unsigned idx;
        if (getSemanticKind(lookupCallee(site)) == SemanticOpKind::ThreadSpawn) {
            // spawn(entry, arg...): entry's parameter i is spawn operand k+1+i.
            idx = firstRealOperand(site) + 1 + i;
        } else {
            idx = i;
        }
        if (idx >= site.getNumOperands()) return nullptr;
        return resolveRoot(site.getOperand(idx), inst->parent, depth + 1);
    }

    void addRootsForModule();
    void processInstance(Instance &inst);
    void addOrphanRoots();
    void writeVerdicts();
};

void QueueOwnershipAnalysisPass::addRootsForModule() {
    // main, once, on the main thread.
    if (auto mainFn = module.lookupSymbol<cir::FuncOp>("main"))
        if (!mainFn.getBody().empty())
            addInstance({mainFn, nextThread++, Mult::One, Mult::One, nullptr, nullptr, false});
    const unsigned mainThread = nextThread ? 0 : nextThread++;

    // Global constructors run once each, on the main thread, before main.
    if (auto ctors = module->getAttrOfType<ArrayAttr>("cir.global_ctors")) {
        for (Attribute a : ctors) {
            auto ctor = dyn_cast<cir::GlobalCtorAttr>(a);
            if (!ctor) continue;
            if (auto f = module.lookupSymbol<cir::FuncOp>(ctor.getName()))
                if (!f.getBody().empty())
                    addInstance({f, mainThread, Mult::One, Mult::One, nullptr, nullptr, false});
        }
    }

    // Address-taken functions may run anywhere, any number of times.
    module.walk([&](cir::FuncOp f) {
        if (f.getBody().empty()) return;
        if (getSemanticKind(f) != SemanticOpKind::Unknown) return;   // shim bodies carry no wtc ops
        auto uses = SymbolTable::getSymbolUses(f, module.getOperation());
        if (!uses) return;
        bool addressTaken = false;
        for (const SymbolTable::SymbolUse &use : *uses) {
            Operation *user = use.getUser();
            if (auto call = dyn_cast<cir::CallOp>(user)) {
                if (call.getCalleeAttr() == use.getSymbolRef()) continue;    // a direct call
                addressTaken = true; break;
            }
            if (isa<QueuePushOp, QueuePopOp, LockOp, UnlockOp>(user)) continue; // fallback_impl
            if (auto gg = dyn_cast<cir::GetGlobalOp>(user)) {
                // Fine only if every consumer, through casts, is a spawn's entry operand.
                bool onlySpawnEntry = true;
                llvm::SmallVector<Value, 4> work{gg.getResult()};
                while (!work.empty() && onlySpawnEntry) {
                    Value v = work.pop_back_val();
                    for (Operation *c : v.getUsers()) {
                        if (isa<cir::CastOp>(c)) { work.push_back(c->getResult(0)); continue; }
                        auto call = dyn_cast<cir::CallOp>(c);
                        if (!call || getSemanticKind(lookupCallee(call)) != SemanticOpKind::ThreadSpawn ||
                            call.getOperand(firstRealOperand(call)) != v) {
                            onlySpawnEntry = false; break;
                        }
                    }
                }
                if (onlySpawnEntry) continue;
                addressTaken = true; break;
            }
            addressTaken = true; break;   // a global initializer, a vtable, anything else
        }
        if (addressTaken && reachesQueueOp(f)) {
            addInstance({f, nextThread++, Mult::Many, Mult::Many, nullptr, nullptr, true});
            f.emitRemark() << "wtc: address of this function is taken; it is treated as callable "
                              "from any thread, any number of times";
            ++remarks;
        }
    });
}

void QueueOwnershipAnalysisPass::processInstance(Instance &inst) {
    // Recursion: the parameter-to-root map would be cyclic. Poison instead.
    for (Instance *p = inst.parent; p; p = p->parent) {
        if (p->func == inst.func) {
            poisonReachable(inst.func);
            return;
        }
    }
    cir::FuncOp f = inst.func;

    f.walk([&](cir::CallOp call) {
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return;                                   // indirect: its targets are roots
        const SemanticOpKind kind = getSemanticKind(callee);
        if (kind == SemanticOpKind::ThreadSpawn) {
            const unsigned k = firstRealOperand(call);
            if (k >= call.getNumOperands()) return;
            cir::FuncOp entry = resolveFunctionOperand(call.getOperand(k));
            if (!entry || entry.getBody().empty()) {
                call.emitRemark() << "wtc: spawn target could not be resolved to a function in this module";
                ++remarks;
                return;
            }
            // A thread that cannot reach a queue operation cannot affect a
            // verdict. Skipping it is not an approximation, and without this
            // the standard library's own call graph fills the worklist.
            if (!reachesQueueOp(entry)) return;
            const Mult m = join(inst.execMult, siteMult(call.getOperation(), f));
            if (instances.size() < kInstanceCap)
                addInstance({entry, nextThread++, m, m, call, &inst, inst.unknownContext});
            else
                capExceeded = true;
            return;
        }
        if (kind != SemanticOpKind::Unknown) return;           // shim functions: nothing inside
        if (callee.getBody().empty()) return;                  // a declaration
        if (!reachesQueueOp(callee)) return;                   // as above
        const Mult m = join(inst.execMult, siteMult(call.getOperation(), f));
        if (instances.size() < kInstanceCap)
            addInstance({callee, inst.thread, inst.threadMult, m, call, &inst, inst.unknownContext});
        else
            capExceeded = true;
    });

    f.walk([&](Operation *op) {
        if (!isa<QueuePushOp, QueuePopOp>(op)) return;
        Value q = isa<QueuePushOp>(op) ? cast<QueuePushOp>(op).getQueue() : cast<QueuePopOp>(op).getQueue();
        Operation *root = resolveRoot(q, &inst);
        if (!root) { poison(op, "the queue object could not be identified"); return; }
        RootInfo &info = roots[root];
        SideInfo &side = isa<QueuePushOp>(op) ? info.producers : info.consumers;
        side.threads.insert(inst.thread);
        if (inst.threadMult == Mult::Many) side.anyMany = true;
        opRoots[op].insert(root);
    });
}

void QueueOwnershipAnalysisPass::addOrphanRoots() {
    // Externally visible functions with a body that nothing here calls may
    // be called by another translation unit, on any thread.
    module.walk([&](cir::FuncOp f) {
        if (f.getBody().empty()) return;
        if (reachedFuncs.count(f.getOperation())) return;
        if (getSemanticKind(f) != SemanticOpKind::Unknown) return;
        const auto linkage = f.getLinkage();
        if (linkage == cir::GlobalLinkageKind::InternalLinkage ||
            linkage == cir::GlobalLinkageKind::PrivateLinkage)
            return;
        if (!reachesQueueOp(f)) return;
        addInstance({f, nextThread++, Mult::Many, Mult::Many, nullptr, nullptr, true});
    });
}

void QueueOwnershipAnalysisPass::writeVerdicts() {
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
    }
    // Poisoned operations that resolved to nothing: unknown, explicitly.
    module.walk([&](Operation *op) {
        if (auto push = dyn_cast<QueuePushOp>(op)) { if (!push.getTopologyAttr()) push.setTopologyAttr(TopologyAttr::get(ctx, Topology::unknown)); }
        else if (auto pop = dyn_cast<QueuePopOp>(op)) { if (!pop.getTopologyAttr()) pop.setTopologyAttr(TopologyAttr::get(ctx, Topology::unknown)); }
    });
}

void QueueOwnershipAnalysisPass::runOnOperation() {
    module = getOperation();
    nextThread = 0;
    instances.clear(); roots.clear(); poisonedTypes.clear(); opRoots.clear();
    unstructuredCache.clear(); reachedFuncs.clear();
    globalPoison = false; capExceeded = false; remarks = 0;

    addRootsForModule();
    // Breadth-first; instances are appended while iterating, in deterministic
    // module order, so the index loop is the queue. Orphan roots are added
    // once the first wave has settled, and the loop resumes where it left
    // off to process them and whatever they reach.
    size_t next = 0;
    for (; next < instances.size(); ++next) processInstance(instances[next]);
    addOrphanRoots();
    for (; next < instances.size(); ++next) processInstance(instances[next]);

    // Two different failures, and telling them apart is the difference
    // between "this program is beyond me" and "this program is too big",
    // which call for different work. The first message hid that.
    if (capExceeded) {
        module.emitRemark() << "wtc: gave up after " << kInstanceCap
                            << " function instances; every queue is unknown";
        ++remarks;
    }
    if (globalPoison) {
        module.emitRemark() << "wtc: an unresolvable queue access had no type to attribute it to; "
                               "every queue is unknown";
        ++remarks;
    }
    writeVerdicts();
    markAllAnalysesPreserved();
}

} // namespace

namespace mlir::wtc {
    void registerQueueOwnershipAnalysisPass() {
        PassRegistration<QueueOwnershipAnalysisPass>();
    }
}
