#include "WTC/Transforms/ThreadInstances.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/IR/SymbolTable.h"

#include "llvm/ADT/SmallVector.h"

using namespace mlir;
using namespace mlir::wtc;

ThreadInstanceWalk::ThreadInstanceWalk(ModuleOp module,
                                       llvm::function_ref<bool(cir::FuncOp)> relevant,
                                       unsigned cap)
    : module_(module), relevant_(relevant), cap_(cap) {}

Instance &ThreadInstanceWalk::addInstance(Instance inst) {
    reachedFuncs_.insert(inst.func.getOperation());
    instances_.push_back(inst);
    return instances_.back();
}

// Whether a spawn's entry operand is a parameter of the function it sits
// in, through casts and single-assignment slots: then a caller decides the
// entry, and only a walk that knows the caller can resolve it.
//
// Only for a function-pointer operand: std::thread's _M_start_thread takes
// `this` where pthread_create takes the entry, and treating that as an
// entry made every caller of every std::thread constructor reach
// everything.
static bool entryComesFromParameter(Value v) {
    auto ptr = dyn_cast<cir::PointerType>(v.getType());
    if (!ptr || !isa<cir::FuncType>(ptr.getPointee())) return false;
    return isa<BlockArgument>(traceToRoot(v));
}

// Computed for the whole module at once, as a fixpoint over the call graph.
// An earlier version recursed and cached "no" for a function while it was
// still being explored, so in a recursive cycle the answer depended on
// where the exploration happened to enter it: one start-up call into a
// recursive group was enough to make a function that does reach the
// predicate answer that it does not, and a pair it touched was reported
// cold.
bool ThreadInstanceWalk::reachesOp(cir::FuncOp f, llvm::function_ref<bool(Operation *)> pred) {
    if (!reachesComputed_) {
        reachesComputed_ = true;
        llvm::DenseMap<Operation *, llvm::SmallVector<Operation *, 4>> callers;
        llvm::SmallVector<Operation *, 64> work;
        module_.walk([&](cir::FuncOp fn) {
            if (fn.getBody().empty()) return;
            bool direct = false;
            fn.walk([&](Operation *op) {
                if (pred(op)) { direct = true; return; }
                auto call = dyn_cast<cir::CallOp>(op);
                if (!call) return;
                cir::FuncOp callee = lookupCallee(call);
                if (!callee) return;
                if (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn) {
                    const unsigned k = spawnEntryOperand(call);
                    if (k >= call.getNumOperands()) return;
                    if (cir::FuncOp entry = resolveFunctionOperand(call.getOperand(k))) {
                        callers[entry.getOperation()].push_back(fn.getOperation());
                    } else if (entryComesFromParameter(call.getOperand(k))) {
                        // Which thread starts here depends on the caller.
                        // Assume it may reach: the cost is an instance, and
                        // the alternative was never seeing memcached's
                        // worker pool, which create_worker(func, arg) starts.
                        direct = true;
                    }
                    return;
                }
                if (!callee.getBody().empty())
                    callers[callee.getOperation()].push_back(fn.getOperation());
            });
            reachesCache_[fn.getOperation()] = direct;
            if (direct) work.push_back(fn.getOperation());
        });
        while (!work.empty()) {
            Operation *g = work.pop_back_val();
            for (Operation *c : callers.lookup(g)) {
                bool &r = reachesCache_[c];
                if (!r) { r = true; work.push_back(c); }
            }
        }
    }
    return reachesCache_.lookup(f.getOperation());
}

bool ThreadInstanceWalk::isUnstructured(cir::FuncOp f) {
    auto it = unstructuredCache_.find(f.getOperation());
    if (it != unstructuredCache_.end()) return it->second;
    bool found = false;
    f.walk([&](Operation *op) {
        if (isa<cir::GotoOp, cir::LabelOp, cir::BrOp, cir::BrCondOp,
                cir::IndirectBrOp, cir::SwitchFlatOp>(op))
            found = true;
    });
    unstructuredCache_[f.getOperation()] = found;
    return found;
}

Mult ThreadInstanceWalk::siteMult(Operation *op, cir::FuncOp f) {
    if (isUnstructured(f)) return Mult::Many;
    for (Operation *p = op->getParentOp(); p && p != f.getOperation(); p = p->getParentOp())
        if (isa<cir::LoopOpInterface>(p)) return Mult::Many;
    return Mult::One;
}

// Whether an indirect call could reach a spawn at all: does this module
// take the address of any function that can start a thread?
//
// Treating every indirect call as a possible spawn is sound and useless.
// In memcached's main the phase ended at the first function pointer, long
// before the option parsing and the restart-file parsing that follow, so
// every field written while starting paired against every worker that
// later read it. If no address-taken function can spawn, an indirect call
// cannot either, and the phase survives to where it actually ends.
bool ThreadInstanceWalk::indirectCallCanSpawn() {
    computeSpawns();
    return *indirectSpawns_;
}

// Whether f, or anything it calls, can start a thread.
bool ThreadInstanceWalk::canSpawn(cir::FuncOp f) {
    computeSpawns();
    return spawnsCache_.lookup(f.getOperation());
}

static bool isAddressTaken(cir::FuncOp f, ModuleOp module) {
    auto uses = SymbolTable::getSymbolUses(f, module.getOperation());
    if (!uses) return false;
    for (const SymbolTable::SymbolUse &use : *uses) {
        auto call = dyn_cast<cir::CallOp>(use.getUser());
        if (call && call.getCalleeAttr() == use.getSymbolRef()) continue;   // a direct call
        return true;
    }
    return false;
}

// Both answers at once, as a fixpoint, for the reason reachesOp gives: a
// recursive cycle must not answer according to where it was entered. The
// indirect-call answer depends on the direct one and feeds back into it, so
// the propagation runs again once it is known.
void ThreadInstanceWalk::computeSpawns() {
    if (indirectSpawns_) return;
    llvm::DenseMap<Operation *, llvm::SmallVector<Operation *, 4>> callers;
    llvm::SmallVector<Operation *, 64> work, indirectCallers;
    module_.walk([&](cir::FuncOp fn) {
        if (fn.getBody().empty()) return;
        bool direct = false, indirect = false;
        fn.walk([&](cir::CallOp call) {
            cir::FuncOp callee = lookupCallee(call);
            if (!callee) { indirect = true; return; }
            if (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn) { direct = true; return; }
            if (!callee.getBody().empty())
                callers[callee.getOperation()].push_back(fn.getOperation());
        });
        spawnsCache_[fn.getOperation()] = direct;
        if (direct) work.push_back(fn.getOperation());
        if (indirect) indirectCallers.push_back(fn.getOperation());
    });
    auto propagate = [&] {
        while (!work.empty()) {
            Operation *g = work.pop_back_val();
            for (Operation *c : callers.lookup(g)) {
                bool &r = spawnsCache_[c];
                if (!r) { r = true; work.push_back(c); }
            }
        }
    };
    propagate();
    bool any = false;
    module_.walk([&](cir::FuncOp fn) {
        if (!any && !fn.getBody().empty() && spawnsCache_.lookup(fn.getOperation()) &&
            isAddressTaken(fn, module_))
            any = true;
    });
    indirectSpawns_ = any;
    if (any) {
        for (Operation *c : indirectCallers) {
            bool &r = spawnsCache_[c];
            if (!r) { r = true; work.push_back(c); }
        }
        propagate();
    }
}

// The operations of f that no spawn can precede. A pre-order walk is program
// order for structured control flow; once a spawning call is seen, nothing
// after it qualifies. An operation inside a loop whose body can spawn does
// not qualify either, because the second lap follows the first spawn.
const llvm::DenseSet<Operation *> &ThreadInstanceWalk::preSpawnOps(cir::FuncOp f) {
    auto it = preSpawnCache_.find(f.getOperation());
    if (it != preSpawnCache_.end()) return it->second;
    llvm::DenseSet<Operation *> pre;
    if (!isUnstructured(f)) {
        bool seen = false;
        f.walk<WalkOrder::PreOrder>([&](Operation *op) {
            if (seen) return WalkResult::advance();
            if (auto loop = dyn_cast<cir::LoopOpInterface>(op)) {
                bool loopSpawns = false;
                op->walk([&](cir::CallOp call) {
                    cir::FuncOp callee = lookupCallee(call);
                    if (!callee ? indirectCallCanSpawn()
                                : (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn ||
                                   (!callee.getBody().empty() && canSpawn(callee)))) {
                        loopSpawns = true; return WalkResult::interrupt();
                    }
                    return WalkResult::advance();
                });
                if (loopSpawns) { seen = true; return WalkResult::advance(); }
            }
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                cir::FuncOp callee = lookupCallee(call);
                if (!callee ? indirectCallCanSpawn()
                            : (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn ||
                               (!callee.getBody().empty() && canSpawn(callee)))) {
                    seen = true;
                    return WalkResult::advance();
                }
            }
            pre.insert(op);
            return WalkResult::advance();
        });
    }
    return preSpawnCache_[f.getOperation()] = std::move(pre);
}

bool ThreadInstanceWalk::spawnPrecedes(Operation *op, cir::FuncOp f) {
    return !preSpawnOps(f).count(op);
}

Operation *ThreadInstanceWalk::resolveRoot(Value v, Instance *inst, unsigned depth) const {
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
        // The entry's parameter i is the i-th argument the spawn passes on.
        idx = spawnFirstArgOperand(site) + i;
    } else {
        idx = i;
    }
    if (idx >= site.getNumOperands()) return nullptr;
    return resolveRoot(site.getOperand(idx), inst->parent, depth + 1);
}

cir::FuncOp ThreadInstanceWalk::resolveEntry(Value v, Instance *inst, unsigned depth) const {
    if (depth > 64 || !inst) return nullptr;
    if (cir::FuncOp f = resolveFunctionOperand(v)) return f;
    Value t = traceToRoot(v);
    if (cir::FuncOp f = resolveFunctionOperand(t)) return f;
    auto arg = dyn_cast<BlockArgument>(t);
    if (!arg || !inst->site) return nullptr;
    if (arg.getOwner() != &inst->func.getBody().front()) return nullptr;
    cir::CallOp site = inst->site;
    unsigned idx = arg.getArgNumber();
    if (getSemanticKind(lookupCallee(site)) == SemanticOpKind::ThreadSpawn)
        idx += spawnFirstArgOperand(site);
    if (idx >= site.getNumOperands()) return nullptr;
    return resolveEntry(site.getOperand(idx), inst->parent, depth + 1);
}

// Whether a function's address, once taken, goes nowhere but into the entry
// operand of a spawn -- directly, or through parameters of functions that
// pass it on, as memcached's create_worker(func, arg) does. Such a function
// runs as the thread that spawn starts, which the walk resolves through the
// call chain; it is not a function that may run anywhere.
static bool flowsOnlyToSpawnEntry(Value v, unsigned depth) {
    if (depth > 8) return false;
    for (OpOperand &use : v.getUses()) {
        Operation *user = use.getOwner();
        if (isa<cir::CastOp>(user)) {
            if (!flowsOnlyToSpawnEntry(user->getResult(0), depth + 1)) return false;
            continue;
        }
        if (auto call = dyn_cast<cir::CallOp>(user)) {
            cir::FuncOp callee = lookupCallee(call);
            if (!callee) return false;
            const unsigned idx = use.getOperandNumber();
            if (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn) {
                if (idx != spawnEntryOperand(call)) return false;
                continue;
            }
            if (getSemanticKind(callee) != SemanticOpKind::Unknown || callee.getBody().empty() ||
                idx >= callee.getNumArguments())
                return false;
            if (!flowsOnlyToSpawnEntry(callee.getArgument(idx), depth + 1)) return false;
            continue;
        }
        // A parameter spilled to its slot at -O0: follow the loads -- but
        // only where resolveEntry will follow them too, which is where
        // traceToRoot takes the slot for a single assignment (one store, in
        // the slot's own block, before every load). With a looser rule here
        // a function assigned to a pointer inside an `if` was declared "only
        // a spawn entry", while the spawn itself could not be resolved, and
        // the thread vanished from the walk altogether.
        if (auto st = dyn_cast<cir::StoreOp>(user)) {
            auto slot = st.getAddr().getDefiningOp<cir::AllocaOp>();
            if (!slot || st.getValue() != v) return false;
            for (Operation *u : slot->getUsers()) {
                if (u == user) continue;
                auto ld = dyn_cast<cir::LoadOp>(u);
                if (!ld || traceToRoot(ld.getResult()) != traceToRoot(v) ||
                    !flowsOnlyToSpawnEntry(ld.getResult(), depth + 1))
                    return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

void ThreadInstanceWalk::addRootsForModule() {
    // A global constructor that starts a thread ends the start-up phase
    // before main begins.
    bool ctorSpawns = false;
    if (auto ctors = module_->getAttrOfType<ArrayAttr>("cir.global_ctors"))
        for (Attribute a : ctors)
            if (auto ctor = dyn_cast<cir::GlobalCtorAttr>(a))
                if (auto f = module_.lookupSymbol<cir::FuncOp>(ctor.getName()))
                    if (!f.getBody().empty() && canSpawn(f)) ctorSpawns = true;

    // main, once, on the main thread.
    if (auto mainFn = module_.lookupSymbol<cir::FuncOp>("main"))
        if (!mainFn.getBody().empty())
            addInstance({mainFn, nextThread_++, Mult::One, Mult::One, nullptr, nullptr, false,
                         !ctorSpawns});
    const unsigned mainThread = nextThread_ ? 0 : nextThread_++;

    // Global constructors run once each, on the main thread, before main.
    // If one of them starts a thread, none of them is taken for start-up:
    // their order is the linker's and the priorities', and a constructor
    // that runs after the spawning one writes while the thread runs.
    if (auto ctors = module_->getAttrOfType<ArrayAttr>("cir.global_ctors")) {
        for (Attribute a : ctors) {
            auto ctor = dyn_cast<cir::GlobalCtorAttr>(a);
            if (!ctor) continue;
            if (auto f = module_.lookupSymbol<cir::FuncOp>(ctor.getName()))
                if (!f.getBody().empty())
                    addInstance({f, mainThread, Mult::One, Mult::One, nullptr, nullptr, false,
                                 !ctorSpawns});
        }
    }

    // Address-taken functions may run anywhere, any number of times.
    module_.walk([&](cir::FuncOp f) {
        if (f.getBody().empty()) return;
        if (getSemanticKind(f) != SemanticOpKind::Unknown) return;   // shim bodies carry no wtc ops
        auto uses = SymbolTable::getSymbolUses(f, module_.getOperation());
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
                if (flowsOnlyToSpawnEntry(gg.getResult(), 0)) continue;
                addressTaken = true; break;
            }
            addressTaken = true; break;   // a global initializer, a vtable, anything else
        }
        if (addressTaken && relevant_(f)) {
            addInstance({f, nextThread_++, Mult::Many, Mult::Many, nullptr, nullptr, true, false});
            f.emitRemark() << "wtc: address of this function is taken; it is treated as callable "
                              "from any thread, any number of times";
            ++remarks_;
        }
    });
}

void ThreadInstanceWalk::expand(Instance &inst) {
    cir::FuncOp f = inst.func;
    f.walk([&](cir::CallOp call) {
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return;                                   // indirect: its targets are roots
        const SemanticOpKind kind = getSemanticKind(callee);
        if (kind == SemanticOpKind::ThreadSpawn) {
            const unsigned k = spawnEntryOperand(call);
            if (k >= call.getNumOperands()) return;
            cir::FuncOp entry = resolveEntry(call.getOperand(k), &inst);
            if (!entry || entry.getBody().empty()) {
                call.emitRemark() << "wtc: spawn target could not be resolved to a function in this module";
                ++remarks_;
                return;
            }
            if (!relevant_(entry)) return;
            const Mult m = join(inst.execMult, siteMult(call.getOperation(), f));
            if (instances_.size() < cap_)
                addInstance({entry, nextThread_++, m, m, call, &inst, inst.speculative, false});
            else
                capExceeded_ = true;
            return;
        }
        if (kind != SemanticOpKind::Unknown) return;           // shim functions: nothing inside
        if (callee.getBody().empty()) return;                  // a declaration
        if (!relevant_(callee)) return;
        const Mult m = join(inst.execMult, siteMult(call.getOperation(), f));
        if (instances_.size() < cap_)
            addInstance({callee, inst.thread, inst.threadMult, m, call, &inst, inst.speculative,
                         inst.startup && !spawnPrecedes(call.getOperation(), f)});
        else
            capExceeded_ = true;
    });
}

void ThreadInstanceWalk::addOrphanRoots() {
    // Externally visible functions with a body that nothing here calls may
    // be called by another translation unit, on any thread.
    module_.walk([&](cir::FuncOp f) {
        if (f.getBody().empty()) return;
        if (reachedFuncs_.count(f.getOperation())) return;
        if (getSemanticKind(f) != SemanticOpKind::Unknown) return;
        const auto linkage = f.getLinkage();
        if (linkage == cir::GlobalLinkageKind::InternalLinkage ||
            linkage == cir::GlobalLinkageKind::PrivateLinkage)
            return;
        if (!relevant_(f)) return;
        addInstance({f, nextThread_++, Mult::Many, Mult::Many, nullptr, nullptr, true, false});
    });
}

void ThreadInstanceWalk::run(llvm::function_ref<void(Instance &)> visit,
                             llvm::function_ref<void(cir::FuncOp)> onRecursion) {
    auto step = [&](Instance &inst) {
        // Recursion: the parameter-to-root map would be cyclic.
        for (Instance *p = inst.parent; p; p = p->parent) {
            if (p->func == inst.func) { onRecursion(inst.func); return; }
        }
        expand(inst);
        visit(inst);
    };

    addRootsForModule();
    // Breadth-first; instances are appended while iterating, in deterministic
    // module order, so the index loop is the queue. Orphan roots are added
    // once the first wave has settled, and the loop resumes where it left
    // off to process them and whatever they reach.
    size_t next = 0;
    for (; next < instances_.size(); ++next) step(instances_[next]);
    addOrphanRoots();
    for (; next < instances_.size(); ++next) step(instances_[next]);
}
