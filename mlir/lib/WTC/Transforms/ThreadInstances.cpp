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
    // A function on a call-graph cycle runs again whenever it recurses, so
    // its accesses repeat however the call that reached it was placed. The
    // walk skips the recursive instance itself, and before this the
    // function counted as running once, which made a pair it touched cold.
    if (isRecursive(inst.func)) inst.execMult = Mult::Many;
    instances_.push_back(inst);
    threadRoot_.try_emplace(inst.thread, &instances_.back());
    return instances_.back();
}

// Whether f can reach itself by direct calls. Computed for every function
// at once from the direct call graph; indirect recursion goes through an
// address-taken root, which already stands for many executions.
bool ThreadInstanceWalk::isRecursive(cir::FuncOp f) {
    if (!recursiveComputed_) {
        recursiveComputed_ = true;
        llvm::DenseMap<Operation *, llvm::SmallVector<Operation *, 4>> callees;
        module_.walk([&](cir::FuncOp fn) {
            if (fn.getBody().empty()) return;
            fn.walk([&](cir::CallOp call) {
                cir::FuncOp callee = lookupCallee(call);
                if (callee && !callee.getBody().empty())
                    callees[fn.getOperation()].push_back(callee.getOperation());
            });
        });
        for (auto &kv : callees) {
            llvm::DenseSet<Operation *> seen;
            llvm::SmallVector<Operation *, 32> work(kv.second.begin(), kv.second.end());
            bool reaches = false;
            while (!work.empty() && !reaches) {
                Operation *g = work.pop_back_val();
                if (g == kv.first) { reaches = true; break; }
                if (!seen.insert(g).second) continue;
                for (Operation *c : callees.lookup(g)) work.push_back(c);
            }
            recursiveCache_[kv.first] = reaches;
        }
    }
    return recursiveCache_.lookup(f.getOperation());
}

unsigned ThreadInstanceWalk::instanceCount(cir::FuncOp f) const {
    unsigned n = 0;
    for (const Instance &inst : instances_) n += inst.func == f;
    return n;
}

bool ThreadInstanceWalk::runsOnce(cir::FuncOp f) const {
    const Instance *only = nullptr;
    for (const Instance &inst : instances_) {
        if (inst.func != f) continue;
        if (only) return false;
        only = &inst;
    }
    return only && only->execMult == Mult::One && only->threadMult == Mult::One &&
           !only->execUnknown;
}

// The chain of (region-holding op, region index) from the function body
// down to op.
static void regionPath(Operation *op, cir::FuncOp f,
                       llvm::SmallVectorImpl<std::pair<Operation *, unsigned>> &path) {
    for (Operation *p = op; p && p != f.getOperation(); p = p->getParentOp()) {
        Region *r = p->getParentRegion();
        Operation *holder = r ? r->getParentOp() : nullptr;
        if (!holder || holder == f.getOperation()) break;
        path.push_back({holder, r->getRegionNumber()});
    }
    std::reverse(path.begin(), path.end());
}

bool ThreadInstanceWalk::exclusive(unsigned threadA, unsigned threadB) const {
    const Instance *a = threadRoot_.lookup(threadA), *b = threadRoot_.lookup(threadB);
    if (!a || !b || !a->site || !b->site) return false;
    // The nearest instance both descend from, and the child of it on each
    // side: the two call sites to compare lie in that instance's function.
    llvm::DenseMap<const Instance *, const Instance *> childOnA;
    for (const Instance *i = a; i->parent; i = i->parent) childOnA[i->parent] = i;
    const Instance *common = nullptr, *childB = nullptr;
    for (const Instance *i = b; i->parent; i = i->parent)
        if (childOnA.count(i->parent)) { common = i->parent; childB = i; break; }
    if (!common) return false;
    // An ancestor that runs more than once runs both arms, one per run.
    if (common->execMult == Mult::Many || common->threadMult == Mult::Many || common->execUnknown)
        return false;
    const Instance *childA = childOnA.lookup(common);
    cir::CallOp siteA = childA->site, siteB = childB->site;
    Operation *ca = siteA.getOperation(), *cb = siteB.getOperation();
    if (ca == cb) return false;
    llvm::SmallVector<std::pair<Operation *, unsigned>, 8> pa, pb;
    regionPath(ca, common->func, pa);
    regionPath(cb, common->func, pb);

    // A loop anywhere above the fork runs both arms, one per iteration.
    auto underLoop = [](const auto &path, size_t upto) {
        for (size_t j = 0; j <= upto && j < path.size(); ++j)
            if (isa<cir::LoopOpInterface>(path[j].first)) return true;
        return false;
    };
    auto lastOf = [](Operation *holder, unsigned region) -> Operation * {
        if (region >= holder->getNumRegions()) return nullptr;
        Region &r = holder->getRegion(region);
        if (r.empty() || r.front().empty()) return nullptr;
        return &r.front().back();
    };
    // The ancestor of op that sits in the same block as `anchor`, so that
    // program order between the two can be read off the block.
    auto siblingOf = [](Operation *op, Operation *anchor) -> Operation * {
        for (Operation *p = op; p; p = p->getParentOp())
            if (p->getBlock() == anchor->getBlock()) return p;
        return nullptr;
    };

    size_t i = 0;
    for (; i < pa.size() && i < pb.size(); ++i) {
        if (pa[i].first != pb[i].first) {
            // Two cases of one switch, each ending in a break, run one or the
            // other; a case that falls through may run both.
            auto caseA = dyn_cast<cir::CaseOp>(pa[i].first), caseB = dyn_cast<cir::CaseOp>(pb[i].first);
            if (caseA && caseB && caseA->getParentOp() == caseB->getParentOp() &&
                isa<cir::SwitchOp>(caseA->getParentOp())) {
                Operation *ea = lastOf(caseA, 0), *eb = lastOf(caseB, 0);
                if (ea && eb && isa<cir::BreakOp, cir::ReturnOp>(ea) &&
                    isa<cir::BreakOp, cir::ReturnOp>(eb))
                    return !underLoop(pa, i);
            }
            return false;
        }
        if (pa[i].second == pb[i].second) continue;
        // Different regions of one op: only an if or a ternary runs exactly
        // one of them, and only outside a loop.
        return isa<cir::IfOp, cir::TernaryOp>(pa[i].first) && !underLoop(pa, i);
    }
    // One site sits under an if the other is not under. They exclude each
    // other if that arm returns from the function and the other site comes
    // after the if: `if (c) { spawn(w); return; } spawn(w);`.
    const bool aDeeper = pa.size() > pb.size();
    const auto &longer = aDeeper ? pa : pb;
    Operation *other = aDeeper ? cb : ca;
    if (i < longer.size() && isa<cir::IfOp>(longer[i].first) && !underLoop(longer, i)) {
        Operation *ifOp = longer[i].first;
        Operation *end = lastOf(ifOp, longer[i].second);
        Operation *after = siblingOf(other, ifOp);
        if (end && isa<cir::ReturnOp>(end) && after && after != ifOp && ifOp->isBeforeInBlock(after))
            return true;
    }
    return false;
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
                    // Resolved as expand() resolves it, through casts and a
                    // single-assignment slot. With casts alone, a helper
                    // that parks the entry in a local and spawns from it
                    // looked irrelevant and was never expanded, and its
                    // thread -- neither a root, since its address flows only
                    // into a spawn, nor an orphan, since it is internal --
                    // was lost without a word.
                    cir::FuncOp entry = resolveFunctionOperand(call.getOperand(k));
                    if (!entry) entry = resolveFunctionOperand(traceToRoot(call.getOperand(k)));
                    if (entry) {
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

bool ThreadInstanceWalk::hasIndirectCall(cir::FuncOp f) {
    bool found = false;
    f.walk([&](cir::CallOp call) { if (!lookupCallee(call)) found = true; });
    return found;
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

Mult ThreadInstanceWalk::siteMult(Operation *op, cir::FuncOp f, bool &known) {
    known = true;
    // A loop is a loop whatever else the function does: an operation inside
    // one repeats, goto or no goto.
    for (Operation *p = op->getParentOp(); p && p != f.getOperation(); p = p->getParentOp())
        if (isa<cir::LoopOpInterface>(p)) return Mult::Many;
    if (isUnstructured(f)) { known = false; return Mult::Many; }
    return Mult::One;
}

Mult ThreadInstanceWalk::siteMult(Operation *op, cir::FuncOp f) {
    bool known;
    return siteMult(op, f, known);
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

static bool flowsOnlyToSpawnEntry(Value v, unsigned depth);

// Whether f may run somewhere the call graph does not show: its address is
// taken, and not merely handed to a spawn as the thread's entry. One rule
// for both askers. The indirect-call check used to count a spawn entry as
// an address taken, so any worker that itself started a thread made every
// indirect call in the module a possible spawn, and main's start-up loop
// before its first function pointer paired against that worker.
static bool isAddressTaken(cir::FuncOp f, ModuleOp module) {
    auto uses = SymbolTable::getSymbolUses(f, module.getOperation());
    if (!uses) return false;
    for (const SymbolTable::SymbolUse &use : *uses) {
        Operation *user = use.getUser();
        if (auto call = dyn_cast<cir::CallOp>(user)) {
            if (call.getCalleeAttr() == use.getSymbolRef()) continue;    // a direct call
            return true;
        }
        if (isa<QueuePushOp, QueuePopOp, LockOp, UnlockOp>(user)) continue; // fallback_impl
        if (auto gg = dyn_cast<cir::GetGlobalOp>(user))
            if (flowsOnlyToSpawnEntry(gg.getResult(), 0)) continue;
        return true;                        // a global initializer, a vtable, anything else
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
    // A function with a goto that cannot start a thread, nor call anything
    // that can, runs entirely before any spawn of its own: every operation
    // in it precedes one. Only where a goto and a spawn meet is the order
    // unreadable, and that case is a guess the clients ask about.
    if (isUnstructured(f) && !canSpawn(f) && !(indirectCallCanSpawn() && hasIndirectCall(f))) {
        f.walk([&](Operation *op) { pre.insert(op); });
        return preSpawnCache_[f.getOperation()] = std::move(pre);
    }
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
                    // The call itself is still before the spawn: what the
                    // callee does before its own spawn is start-up too, and
                    // the instance it creates reads that from this set.
                    pre.insert(op);
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

Value ThreadInstanceWalk::singleStoredGlobalValue(cir::GlobalOp g) const {
    if (!globalStoresComputed_) {
        globalStoresComputed_ = true;
        // One pass over every use of every global: the stores through its
        // address, and whether its address goes anywhere but a load.
        llvm::DenseMap<Operation *, llvm::SmallVector<cir::StoreOp, 2>> stores;
        llvm::DenseSet<Operation *> spoiled;
        ModuleOp module = module_;   // a copy of the handle: walk is not const
        // A name used anywhere but in a cir.get_global -- a global_view in
        // another global's initializer, an alias -- is a second way to the
        // storage, and a store through it would not be seen below.
        auto spoilName = [&](Operation *from, StringAttr name) {
            if (auto g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(from, name))
                spoiled.insert(g.getOperation());
        };
        if (auto uses = SymbolTable::getSymbolUses(module.getOperation()))
            for (const SymbolTable::SymbolUse &use : *uses) {
                if (isa<cir::GetGlobalOp>(use.getUser())) continue;
                spoilName(use.getUser(), use.getSymbolRef().getRootReference());
            }
        module.walk([&](cir::GlobalOp g) {
            if (auto alias = g.getAliasee()) spoilName(g, StringAttr::get(g.getContext(), *alias));
            if (auto init = g.getInitialValue())
                init->walk([&](SymbolRefAttr ref) { spoilName(g, ref.getRootReference()); });
        });
        module.walk([&](cir::GetGlobalOp gg) {
            auto global = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr());
            if (!global || !isa<cir::PointerType>(global.getSymType())) return;
            Operation *key = global.getOperation();
            llvm::SmallVector<Value, 4> work{gg.getResult()};
            while (!work.empty()) {
                Value p = work.pop_back_val();
                for (OpOperand &use : p.getUses()) {
                    Operation *u = use.getOwner();
                    if (isa<cir::LoadOp>(u)) continue;
                    if (auto st = dyn_cast<cir::StoreOp>(u)) {
                        if (st.getAddr() == p) stores[key].push_back(st);
                        else spoiled.insert(key);               // the address itself is stored
                        continue;
                    }
                    if (auto c = dyn_cast<cir::CastOp>(u)) {
                        if (c.getKind() == cir::CastKind::bitcast) { work.push_back(c.getResult()); continue; }
                    }
                    spoiled.insert(key);
                }
            }
        });
        for (auto &kv : stores) {
            if (spoiled.count(kv.first)) continue;
            if (kv.second.size() == 1) globalStores_[kv.first] = kv.second[0].getValue();
            globalMultiStores_[kv.first] = kv.second;
        }
    }
    // An initialiser other than null is a second value.
    if (auto init = g.getInitialValue()) {
        auto cp = dyn_cast<cir::ConstPtrAttr>(*init);
        if (!cp || !cp.getValue().getValue().isZero()) return Value();
    }
    return globalStores_.lookup(g.getOperation());
}

static Operation *enclosingLoop(Operation *op, cir::FuncOp f) {
    for (Operation *p = op->getParentOp(); p && p != f.getOperation(); p = p->getParentOp())
        if (isa<cir::LoopOpInterface>(p)) return p;
    return nullptr;
}

bool ThreadInstanceWalk::isInductionOf(Value index, Operation *loop, bool &unitStep) {
    unitStep = false;
    auto forOp = dyn_cast<cir::ForOp>(loop);
    if (!forOp) return false;
    // Through integer conversions to the load of a slot.
    Value v = index;
    for (unsigned guard = 0; guard < 16; ++guard) {
        if (auto c = v.getDefiningOp<cir::CastOp>()) {
            if (c.getKind() != cir::CastKind::integral) return false;
            v = c.getSrc(); continue;
        }
        break;
    }
    auto ld = v.getDefiningOp<cir::LoadOp>();
    if (!ld) return false;
    auto slot = traceToLocalRoot(ld.getAddr()).getDefiningOp<cir::AllocaOp>();
    if (!slot) return false;
    Region &step = forOp.getStep();
    bool stepped = false;
    unitStep = true;
    for (Operation *u : slot->getUsers()) {
        if (isa<cir::LoadOp>(u)) continue;
        auto st = dyn_cast<cir::StoreOp>(u);
        if (!st || st.getValue() == slot.getResult()) return false;
        if (step.isAncestor(u->getParentRegion())) {
            // One step, by one: i++ or i--, or i += 1. Anything else, and
            // the elements the loop hands out are not neighbours.
            if (stepped) unitStep = false;
            stepped = true;
            auto loadsSlot = [&](Value x) {
                auto l = traceToLocalRoot(x).getDefiningOp<cir::LoadOp>();
                return l && traceToLocalRoot(l.getAddr()) == slot.getResult();
            };
            Value nv = st.getValue();
            bool unit = false;
            if (auto inc = nv.getDefiningOp<cir::IncOp>()) unit = loadsSlot(inc.getInput());
            else if (auto dec = nv.getDefiningOp<cir::DecOp>()) unit = loadsSlot(dec.getInput());
            else {
                Value lhs, rhs;
                if (auto add = nv.getDefiningOp<cir::AddOp>()) { lhs = add.getLhs(); rhs = add.getRhs(); }
                else if (auto sub = nv.getDefiningOp<cir::SubOp>()) { lhs = sub.getLhs(); rhs = sub.getRhs(); }
                if (rhs) {
                    auto c = rhs.getDefiningOp<cir::ConstantOp>();
                    auto k = c ? dyn_cast<cir::IntAttr>(c.getValue()) : cir::IntAttr();
                    unit = k && k.getValue() == 1 && loadsSlot(lhs);
                }
            }
            if (!unit) unitStep = false;
            continue;
        }
        if (loop->isAncestor(u)) return false;        // written in the body or the condition
    }
    if (!stepped) unitStep = false;
    return stepped;
}

// Whether every iteration of the loop reaches `op` once: op sits in the
// loop's body under nothing but scopes, and the body has no early exit or
// jump that could skip it on some iterations.
static bool unconditionalInBody(Operation *op, cir::ForOp loop) {
    for (Operation *p = op->getParentOp(); p != loop.getOperation(); p = p->getParentOp())
        if (!p || !isa<cir::ScopeOp>(p)) return false;
    if (!loop.getBody().isAncestor(op->getParentRegion())) return false;
    bool jumps = false;
    loop.getBody().walk([&](Operation *o) {
        if (isa<cir::ContinueOp, cir::BreakOp, cir::GotoOp, cir::ReturnOp>(o)) jumps = true;
    });
    return !jumps;
}

const DataLayout &ThreadInstanceWalk::dataLayout() const {
    if (!dataLayout_) dataLayout_.emplace(module_);
    return *dataLayout_;
}

Operation *ThreadInstanceWalk::resolveRoot(Value v, Instance *inst, unsigned depth) const {
    return resolveRootImpl(v, inst, depth, cir::CallOp(), nullptr, nullptr);
}

// An element step the walk could not read under a per-thread one: which
// element is not known, so neither is whose it is.
static void settle(Place &place) {
    if (place.unknownIndex) { place.loop = nullptr; place.elem = Type(); place.owner = false; }
}

Place ThreadInstanceWalk::resolvePlace(Value v, Instance *inst) const {
    Place place;
    place.root = resolveRootImpl(v, inst, 0, cir::CallOp(), &place, nullptr);
    if (!place.root) {
        const Unplaced why = place.cause;
        place = Place();
        place.cause = why;
    }
    settle(place);
    return place;
}

llvm::SmallVector<Place, 4> ThreadInstanceWalk::resolvePlaces(Value v, Instance *inst) const {
    llvm::SmallVector<Place, 4> many;
    Place place;
    place.root = resolveRootImpl(v, inst, 0, cir::CallOp(), &place, &many);
    llvm::SmallVector<Place, 4> out;
    if (place.root) {
        out.push_back(place);
    } else if (!many.empty()) {
        out = std::move(many);
    } else {
        Place none;
        none.cause = place.cause == Unplaced::None ? Unplaced::Other : place.cause;
        out.push_back(none);
    }
    for (Place &p : out) settle(p);
    return out;
}

// Adds a place to a set unless an equal one is there: two stores of the
// same handle, one on each arm of an `if`, are one place.
static void addPlace(llvm::SmallVectorImpl<Place> &out, const Place &p) {
    for (const Place &q : out)
        if (q.root == p.root && q.offset == p.offset && q.unknownIndex == p.unknownIndex && q.loop == p.loop)
            return;
    out.push_back(p);
}

static bool isNullPointer(Value v) {
    if (auto c = v.getDefiningOp<cir::ConstantOp>())
        if (auto cp = dyn_cast<cir::ConstPtrAttr>(c.getValue()))
            return cp.getValue().getValue().isZero();
    return false;
}

// A constant that names no object: zeros, integers, floats, null pointers,
// and records and arrays of those. A record written from one -- `c = {0}`,
// `c = { .n = 8 }`, which the front end emits as a copy out of a private
// constant -- holds no address a field listing would want, as a memset
// leaves none. A view of a global, or a pointer of any other value, does.
static bool namesNoObject(Attribute a) {
    if (!a) return true;
    if (isa<cir::ZeroAttr, cir::UndefAttr, cir::PoisonAttr, cir::IntAttr, cir::FPAttr, cir::BoolAttr>(a))
        return true;
    if (auto p = dyn_cast<cir::ConstPtrAttr>(a)) return p.getValue().getValue().isZero();
    if (auto r = dyn_cast<cir::ConstRecordAttr>(a)) {
        for (Attribute m : r.getMembers())
            if (!namesNoObject(m)) return false;
        return true;
    }
    if (auto arr = dyn_cast<cir::ConstArrayAttr>(a)) {
        if (isa<StringAttr>(arr.getElts())) return true;
        auto elts = dyn_cast<ArrayAttr>(arr.getElts());
        if (!elts) return false;
        for (Attribute e : elts)
            if (!namesNoObject(e)) return false;
        return true;
    }
    return false;
}

// Whether what a copy reads from is a constant global that names no object.
static bool copiesNoObject(Value src) {
    auto gg = traceToLocalRoot(src).getDefiningOp<cir::GetGlobalOp>();
    if (!gg) return false;
    auto g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr());
    if (!g || !g.getConstant()) return false;
    auto init = g.getInitialValue();
    return !init || namesNoObject(*init);
}

// Whether a store writes a constant record that names no object.
static bool storesNoObject(cir::StoreOp st) {
    auto c = st.getValue().getDefiningOp<cir::ConstantOp>();
    return c && namesNoObject(c.getValue());
}

llvm::ArrayRef<Instance *> ThreadInstanceWalk::instancesOf(cir::FuncOp f) const {
    if (instancesIndexed_ != instances_.size()) {
        instancesByFunc_.clear();
        // Resolved under new instances, a field's candidates may differ.
        fieldCandidates_.clear();
        globalCandidates_.clear();
        slotCandidates_.clear();
        for (const Instance &inst : instances_) {
            cir::FuncOp fn = inst.func;
            instancesByFunc_[fn.getOperation()].push_back(const_cast<Instance *>(&inst));
        }
        instancesIndexed_ = instances_.size();
    }
    auto it = instancesByFunc_.find(f.getOperation());
    if (it == instancesByFunc_.end()) return {};
    return it->second;
}

// Every store into every field of the module, by (record, field), and the
// fields no such list can speak for: the field's address goes somewhere a
// store through it would not be seen (a call, a store of the address, a
// cast to anything but a reinterpretation), or the record it is in -- or a
// record containing that record -- is written as a whole: assigned, copied,
// or handed by pointer to a function with no body in the module.
void ThreadInstanceWalk::computeFieldStores() const {
    fieldStoresComputed_ = true;
    ModuleOp module = module_;
    llvm::DenseSet<Type> whole;   // records written as a whole somewhere
    auto recordIn = [](Type t) -> Type {
        while (auto arr = dyn_cast_or_null<cir::ArrayType>(t)) t = arr.getElementType();
        return isa_and_nonnull<cir::RecordType>(t) ? t : Type();
    };
    auto recordBehind = [&](Type t) -> Type {
        auto pt = dyn_cast<cir::PointerType>(t);
        return pt ? recordIn(pt.getPointee()) : Type();
    };
    auto fieldKey = [](cir::GetMemberOp gm) -> std::optional<FieldKey> {
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
        if (!rec || rec.isIncomplete()) return std::nullopt;
        return FieldKey{rec, gm.getIndex()};
    };
    // Functions with no body that read what they are handed and write
    // nothing the walk would want to know of: a memset writes zeros, and
    // a null names no object.
    auto harmless = [](cir::FuncOp f) {
        if (!f) return false;
        if (isAllocator(f) || getSemanticKind(f) == SemanticOpKind::ThreadSpawn) return true;
        const StringRef n = f.getSymName();
        return n == "free" || n == "memset" || n == "memcmp" || n == "strlen" || n == "strcmp" ||
               n == "strncmp" || n == "memchr" || n == "puts" || n == "printf" || n == "fprintf";
    };
    module.walk([&](Operation *op) {
        if (auto gm = dyn_cast<cir::GetMemberOp>(op)) {
            auto key = fieldKey(gm);
            if (!key) return;
            FieldStores &fs = fieldStores_[*key];
            // A union: a store through any member writes every member, so
            // no member's list is complete, and a record member is
            // written whole.
            if (cast<cir::RecordType>(key->first).isUnion()) {
                fs.poisoned = true;
                for (Type m : cast<cir::RecordType>(key->first).getMembers())
                    if (Type rec = recordIn(m)) whole.insert(rec);
                return;
            }
            llvm::SmallVector<Value, 4> work{gm.getResult()};
            while (!work.empty()) {
                Value p = work.pop_back_val();
                for (OpOperand &use : p.getUses()) {
                    Operation *u = use.getOwner();
                    if (isa<cir::LoadOp>(u)) continue;
                    if (auto st = dyn_cast<cir::StoreOp>(u)) {
                        if (st.getAddr() != p) { fs.poisoned = true; continue; }   // the address is published
                        if (isa<cir::RecordType>(st.getValue().getType())) {
                            if (storesNoObject(st)) continue;
                            whole.insert(st.getValue().getType());
                            fs.poisoned = true;
                            continue;
                        }
                        if (isNullPointer(st.getValue())) continue;
                        fs.stores.push_back(st);
                        continue;
                    }
                    if (auto c = dyn_cast<cir::CastOp>(u))
                        if (c.getKind() == cir::CastKind::bitcast) { work.push_back(c.getResult()); continue; }
                    // A part of a record or array field is touched, not the
                    // field: nothing to a pointer-typed field, which has
                    // no parts.
                    if (isa<cir::GetMemberOp, cir::GetElementOp, cir::PtrStrideOp>(u)) continue;
                    if (auto c = dyn_cast<cir::CastOp>(u))
                        if (c.getKind() == cir::CastKind::array_to_ptrdecay) continue;
                    if (auto ms = dyn_cast<cir::MemSetOp>(u))
                        if (ms.getDst() == p) continue;   // zeros: no object named
                    bool copied = false;
                    Value from;
                    if (auto cp = dyn_cast<cir::CopyOp>(u)) { copied = cp.getDst() == p; from = cp.getSrc(); }
                    if (auto mc = dyn_cast<cir::MemCpyOp>(u)) { copied = mc.getDst() == p; from = mc.getSrc(); }
                    if (auto mm = dyn_cast<cir::MemMoveOp>(u)) { copied = mm.getDst() == p; from = mm.getSrc(); }
                    if (copied && copiesNoObject(from)) continue;   // `= {0}`: no object named
                    if (copied)
                        if (Type rec = recordBehind(p.getType())) whole.insert(rec);
                    fs.poisoned = true;   // a call, a copy, anything else
                }
            }
            return;
        }
        // A record written as a whole, wherever its address came from.
        if (auto st = dyn_cast<cir::StoreOp>(op)) {
            if (isa<cir::RecordType>(st.getValue().getType()) && !storesNoObject(st))
                whole.insert(st.getValue().getType());
            return;
        }
        // A global record with an initialiser that names an object: its
        // fields hold values no store lists.
        if (auto g = dyn_cast<cir::GlobalOp>(op)) {
            if (auto init = g.getInitialValue())
                if (!namesNoObject(*init))
                    if (Type rec = recordIn(g.getSymType())) whole.insert(rec);
            return;
        }
        // The record seen through another type -- `(char *)&c`, `(void *)
        // c`, `(T *)&c` -- is written in ways no field listing sees: a store
        // through the view, the view stored or handed to code with a body
        // of its own or none at all. Only a load, a memset, a copy out of
        // it and a call to a function known to write nothing leave the
        // listing standing.
        if (auto c = dyn_cast<cir::CastOp>(op)) {
            if (c.getKind() != cir::CastKind::bitcast) return;
            Type rec = recordBehind(c.getSrc().getType());
            if (!rec || recordBehind(c.getResult().getType()) == rec) return;
            llvm::SmallVector<Value, 4> work{c.getResult()};
            bool written = false;
            while (!work.empty() && !written) {
                Value p = work.pop_back_val();
                for (OpOperand &use : p.getUses()) {
                    Operation *u = use.getOwner();
                    if (isa<cir::LoadOp>(u)) continue;
                    if (auto cc = dyn_cast<cir::CastOp>(u))
                        if (cc.getKind() == cir::CastKind::bitcast) { work.push_back(cc.getResult()); continue; }
                    if (auto ms = dyn_cast<cir::MemSetOp>(u))
                        if (ms.getDst() == p) continue;
                    if (auto mc = dyn_cast<cir::MemCpyOp>(u))
                        if (mc.getSrc() == p && mc.getDst() != p) continue;
                    if (auto mm = dyn_cast<cir::MemMoveOp>(u))
                        if (mm.getSrc() == p && mm.getDst() != p) continue;
                    if (auto call = dyn_cast<cir::CallOp>(u))
                        if (harmless(lookupCallee(call))) continue;
                    written = true;
                    break;
                }
            }
            if (written) whole.insert(rec);
            return;
        }
        Value dst, src;
        if (auto cp = dyn_cast<cir::CopyOp>(op)) { dst = cp.getDst(); src = cp.getSrc(); }
        else if (auto mc = dyn_cast<cir::MemCpyOp>(op)) { dst = mc.getDst(); src = mc.getSrc(); }
        else if (auto mm = dyn_cast<cir::MemMoveOp>(op)) { dst = mm.getDst(); src = mm.getSrc(); }
        if (dst) {
            if (copiesNoObject(src)) return;
            if (Type rec = recordBehind(traceToLocalRoot(dst).getType())) whole.insert(rec);
            return;
        }
        // A record handed by pointer to code the module cannot see may
        // come back with any of its fields rewritten.
        if (auto call = dyn_cast<cir::CallOp>(op)) {
            cir::FuncOp callee = lookupCallee(call);
            if (callee && !callee.getBody().empty()) return;
            if (harmless(callee)) return;
            for (Value arg : call.getOperands())
                if (Type rec = recordBehind(traceToRoot(arg).getType())) whole.insert(rec);
        }
    });
    // A record written as a whole takes every record nested in it along.
    llvm::SmallVector<Type, 8> work(whole.begin(), whole.end());
    while (!work.empty()) {
        auto rec = dyn_cast<cir::RecordType>(work.pop_back_val());
        if (!rec || rec.isIncomplete()) continue;
        for (Type m : rec.getMembers()) {
            while (auto arr = dyn_cast<cir::ArrayType>(m)) m = arr.getElementType();
            if (isa<cir::RecordType>(m) && whole.insert(m).second) work.push_back(m);
        }
    }
    for (auto &kv : fieldStores_)
        if (whole.count(kv.first.first)) kv.second.poisoned = true;
}

bool ThreadInstanceWalk::resolveSources(llvm::ArrayRef<cir::StoreOp> stores, Instance *ctx, bool perInstance,
                                        unsigned depth, llvm::SmallVectorImpl<Place> &out,
                                        Unplaced &why) const {
    for (cir::StoreOp st : stores) {
        llvm::SmallVector<Instance *, 4> contexts;
        if (perInstance) {
            auto f = st->getParentOfType<cir::FuncOp>();
            llvm::ArrayRef<Instance *> insts = f ? instancesOf(f) : llvm::ArrayRef<Instance *>();
            if (insts.size() > kMaxStoreInstances) { why = Unplaced::ManySources; return false; }
            contexts.assign(insts.begin(), insts.end());
            // A store in a function the walk never entered -- an init()
            // that touches no field -- is resolved without a call string.
            if (contexts.empty()) contexts.push_back(nullptr);
        } else {
            contexts.push_back(ctx);
        }
        for (Instance *c : contexts) {
            Place sub;
            llvm::SmallVector<Place, 4> subMany;
            Operation *root = resolveRootImpl(st.getValue(), c, depth + 1, cir::CallOp(), &sub, &subMany);
            // An element the storing thread owns is, to the loading thread,
            // some element: the handle was published, and which thread
            // stored it is not what the loading thread's spawn says.
            auto strip = [&](Place &p) {
                if (perInstance && p.loop) p.unknownIndex = true;
            };
            if (root) {
                sub.root = root;
                strip(sub);
                addPlace(out, sub);
            } else if (!subMany.empty()) {
                for (Place p : subMany) { strip(p); addPlace(out, p); }
            } else {
                // One source nobody can place: the set says nothing, and
                // the reason is that source's, the frontier of what the
                // walk reaches, not the load's.
                if (sub.cause != Unplaced::None) why = sub.cause;
                return false;
            }
            if (out.size() > kMaxSources) { why = Unplaced::ManySources; return false; }
        }
    }
    return !out.empty();
}

bool ThreadInstanceWalk::placesOfLoad(Value at, Instance *inst, unsigned depth,
                                      llvm::SmallVectorImpl<Place> &out, Unplaced &why) const {
    if (auto slot = at.getDefiningOp<cir::AllocaOp>()) {
        // Every store into the slot, under its own name or a reinterpretation
        // of it, in this very instance; any other use of the address -- a
        // call, a store of the address, a step from it -- is a name for the
        // slot the listing would not see, and ends it. (A review found a
        // store through a bitcast of the slot admitted by the escape check
        // and missing from the list.)
        (void)instancesOf(cir::FuncOp());   // refresh the index, and the memos with it
        const std::pair<Operation *, const Instance *> slotKey{slot.getOperation(), inst};
        if (slotCandidates_[slotKey].inProgress) return false;   // `p = p->next`: set from itself
        if (!slotCandidates_[slotKey].ok && slotCandidates_[slotKey].why == Unplaced::None) {
            slotCandidates_[slotKey].inProgress = true;
            llvm::SmallVector<cir::StoreOp, 4> stores;
            bool listed = true;
            llvm::SmallVector<Value, 4> work{slot.getResult()};
            while (!work.empty() && listed) {
                Value p = work.pop_back_val();
                for (OpOperand &use : p.getUses()) {
                    Operation *u = use.getOwner();
                    if (isa<cir::LoadOp>(u)) continue;
                    if (isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
                    if (auto st = dyn_cast<cir::StoreOp>(u)) {
                        if (st.getAddr() != p) { listed = false; break; }
                        if (!isNullPointer(st.getValue())) stores.push_back(st);
                        continue;
                    }
                    if (auto c = dyn_cast<cir::CastOp>(u))
                        if (c.getKind() == cir::CastKind::bitcast) { work.push_back(c.getResult()); continue; }
                    listed = false;
                    break;
                }
            }
            llvm::SmallVector<Place, 4> places;
            Unplaced w = why;
            const bool ok = listed && resolveSources(stores, inst, false, depth, places, w);
            Candidates &m = slotCandidates_[slotKey];
            m.inProgress = false;
            m.ok = ok;
            m.places = std::move(places);
            m.why = ok ? Unplaced::None : w;
        }
        const Candidates &m = slotCandidates_[slotKey];
        if (!m.ok) {
            if (m.why != Unplaced::None) why = m.why;
            return false;
        }
        for (const Place &p : m.places) addPlace(out, p);
        return true;
    }
    llvm::ArrayRef<cir::StoreOp> stores;
    Operation *globalKey = nullptr;
    FieldKey fieldKey;
    if (auto gg = at.getDefiningOp<cir::GetGlobalOp>()) {
        auto g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr());
        if (!g) return false;
        singleStoredGlobalValue(g);   // builds the store lists
        if (auto init = g.getInitialValue()) {
            auto cp = dyn_cast<cir::ConstPtrAttr>(*init);
            if (!cp || !cp.getValue().getValue().isZero()) return false;   // a value nobody stored
        }
        auto it = globalMultiStores_.find(g.getOperation());
        if (it == globalMultiStores_.end()) return false;
        stores = it->second;
        globalKey = g.getOperation();
    } else if (auto gm = at.getDefiningOp<cir::GetMemberOp>()) {
        if (!fieldStoresComputed_) computeFieldStores();
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
        if (!rec || rec.isIncomplete()) return false;
        fieldKey = FieldKey{rec, gm.getIndex()};
        auto it = fieldStores_.find(fieldKey);
        if (it == fieldStores_.end() || it->second.poisoned) return false;
        stores = it->second.stores;
    } else {
        return false;
    }
    (void)instancesOf(cir::FuncOp());   // refresh the index, and the memo with it
    // Looked up afresh after the nested resolution: a query inside it may
    // have grown the map.
    auto memo = [&]() -> Candidates & {
        return globalKey ? globalCandidates_[globalKey] : fieldCandidates_[fieldKey];
    };
    if (memo().inProgress) return false;   // a cycle: the field is set from itself
    if (!memo().ok && memo().why == Unplaced::None) {
        memo().inProgress = true;
        llvm::SmallVector<Place, 4> places;
        Unplaced w = why;
        // Null stores name no object; a list of only those is empty.
        llvm::SmallVector<cir::StoreOp, 4> named;
        for (cir::StoreOp st : stores)
            if (!isNullPointer(st.getValue())) named.push_back(st);
        const bool ok = resolveSources(named, nullptr, true, depth, places, w);
        Candidates &m = memo();
        m.inProgress = false;
        m.ok = ok;
        m.places = std::move(places);
        m.why = ok ? Unplaced::None : w;
    }
    const Candidates &m = memo();
    if (!m.ok) {
        if (m.why != Unplaced::None) why = m.why;
        return false;
    }
    for (const Place &p : m.places) addPlace(out, p);
    return true;
}

Operation *ThreadInstanceWalk::resolveRootImpl(Value v, Instance *inst, unsigned depth, cir::CallOp via,
                                               Place *place, llvm::SmallVectorImpl<Place> *many) const {
    auto fail = [&](Unplaced why) -> Operation * {
        if (place && place->cause == Unplaced::None) place->cause = why;
        return nullptr;
    };
    if (depth > 64) return fail(Unplaced::Other);
    Value t = traceToRoot(v);
    // A member's address handed on -- `&r->prod` as the argument of the
    // routine that moves a queue's head -- is so many bytes into the record
    // it is a member of; a union's members all start where the union does.
    if (auto gm = t.getDefiningOp<cir::GetMemberOp>()) {
        if (!place) return fail(Unplaced::Other);
        auto ptrTy = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        auto rec = ptrTy ? dyn_cast<cir::RecordType>(ptrTy.getPointee()) : cir::RecordType();
        if (!rec || rec.isIncomplete()) return fail(Unplaced::Other);
        place->offset += rec.getElementOffset(dataLayout(), gm.getIndex());
        return resolveRootImpl(gm.getAddr(), inst, depth + 1, via, place, many);
    }
    // An element step. A constant index is so many bytes in; the
    // induction variable of the loop that spawned this thread, at the
    // spawn, means each thread the spawn starts gets its own element;
    // anything else is some element of the array.
    Value stepIndex, stepBase;
    Type stepElem;
    if (auto ps = t.getDefiningOp<cir::PtrStrideOp>()) {
        stepIndex = ps.getStride(); stepBase = ps.getBase();
        stepElem = cast<cir::PointerType>(ps.getBase().getType()).getPointee();
    } else if (auto ge = t.getDefiningOp<cir::GetElementOp>()) {
        stepIndex = ge.getIndex(); stepBase = ge.getBase();
        if (auto arr = dyn_cast<cir::ArrayType>(cast<cir::PointerType>(ge.getBase().getType()).getPointee()))
            stepElem = arr.getElementType();
    }
    if (stepBase) {
        if (!place || !stepElem || isa<cir::VoidType, cir::FuncType>(stepElem)) return fail(Unplaced::Other);
        const uint64_t elemSize = dataLayout().getTypeSize(stepElem);
        auto c = traceToLocalRoot(stepIndex).getDefiningOp<cir::ConstantOp>();
        auto k = c ? dyn_cast<cir::IntAttr>(c.getValue()) : cir::IntAttr();
        if (k && !k.getValue().isNegative() && k.getValue().getActiveBits() <= 32) {
            place->offset += k.getValue().getZExtValue() * elemSize;
            return resolveRootImpl(stepBase, inst, depth + 1, via, place, many);
        }
        Operation *loop = via ? enclosingLoop(via.getOperation(), inst->func) : nullptr;
        bool unitStep = false;
        if (loop && loop->isAncestor(t.getDefiningOp()) && isInductionOf(stepIndex, loop, unitStep)) {
            place->loop = loop;
            place->elem = stepElem;
            // Neighbouring elements both have a thread only when the loop
            // walks the array one element at a time, spawns on every
            // iteration, and is the only repetition around the spawn.
            auto forOp = dyn_cast<cir::ForOp>(loop);
            place->owner = unitStep && forOp && unconditionalInBody(via.getOperation(), forOp) &&
                           !enclosingLoop(loop, inst->func) && inst->execMult == Mult::One &&
                           !inst->execUnknown && inst->threadMult == Mult::One && !inst->recurring;
            return resolveRootImpl(stepBase, inst, depth + 1, cir::CallOp(), place, many);
        }
        place->unknownIndex = true;
        return resolveRootImpl(stepBase, inst, depth + 1, cir::CallOp(), place, many);
    }
    // The object an allocator returned: the allocator call is its identity,
    // behind any wrapper that hands it back -- the same object may also
    // leave the wrapper by a global it stores, and two names for it must be
    // one root. How many times it is made is read from the call graph
    // (siteRunsOnce), not from the wrapper's own loop structure alone.
    // A wrapper that hands back one of several allocations, as a
    // constructor choosing between allocators does, hands back one of
    // several objects: the steps taken so far apply to each, and none is
    // owned.
    auto oneOfSites = [&](llvm::ArrayRef<cir::CallOp> sites) -> Operation * {
        if (sites.size() == 1) { cir::CallOp s = sites.front(); return s.getOperation(); }
        if (sites.empty() || !many || !place) return fail(Unplaced::CallResult);
        if (sites.size() > kMaxSources) return fail(Unplaced::ManySources);
        for (cir::CallOp s : sites) {
            Place p = *place;
            p.root = s.getOperation();
            p.owner = false;
            p.oneOf = true;
            many->push_back(p);
        }
        return nullptr;
    };
    if (auto call = t.getDefiningOp<cir::CallOp>())
        return oneOfSites(allocationsBehind(call));
    // A pointer loaded from a global the module writes exactly once: the
    // loaded value is the one stored, wherever the load is. The stored
    // value is resolved without an instance, since the storing function's
    // instance is not this one: a global, an allocation, nothing else.
    if (auto ld = t.getDefiningOp<cir::LoadOp>()) {
        Value at = traceToLocalRoot(ld.getAddr());
        Unplaced why = Unplaced::Other;
        if (auto gg = at.getDefiningOp<cir::GetGlobalOp>()) {
            why = Unplaced::LoadedGlobal;
            auto g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr());
            if (Value stored = g ? singleStoredGlobalValue(g) : Value()) {
                Value s = traceToRoot(stored);
                if (auto call = s.getDefiningOp<cir::CallOp>()) {
                    llvm::SmallVector<cir::CallOp, 4> sites = allocationsBehind(call);
                    if (!sites.empty()) return oneOfSites(sites);
                    if (!many) return fail(Unplaced::CallResult);
                } else if (auto sg = s.getDefiningOp<cir::GetGlobalOp>()) {
                    return SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(sg, sg.getNameAttr()).getOperation();
                } else if (!many) {
                    return fail(Unplaced::Other);
                }
            }
        } else if (at.getDefiningOp<cir::GetMemberOp>()) {
            why = Unplaced::LoadedField;
        } else if (at.getDefiningOp<cir::AllocaOp>()) {
            why = Unplaced::LoadedSlot;
        }
        if (!many || !place || why == Unplaced::Other) return fail(why);
        // Written in several places, each of which the walk can place: the
        // handle is one of those places. The steps taken so far -- a
        // constant index, an element the thread owns -- apply to each.
        llvm::SmallVector<Place, 4> places;
        if (!placesOfLoad(at, inst, depth, places, why)) return fail(why);
        for (Place &p : places) {
            p.offset += place->offset;
            if (place->unknownIndex) p.unknownIndex = true;
            if (place->loop) {
                if (p.loop && p.loop != place->loop) p.unknownIndex = true;
                else { p.loop = place->loop; p.elem = place->elem; }
            }
            p.owner = false;
            p.oneOf = true;
        }
        if (places.size() == 1) {
            // One place under every store: not one of, but that one.
            *place = places.front();
            place->oneOf = false;
            return place->root;
        }
        many->append(places.begin(), places.end());
        return nullptr;
    }
    if (auto gg = t.getDefiningOp<cir::GetGlobalOp>()) {
        auto g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr());
        // An alias has no storage of its own: the object is the aliasee's,
        // and two names for one storage must be one root, or a pair split
        // across the two names is missed.
        for (unsigned hop = 0; g && g.getAliasee() && hop < 8; ++hop)
            g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(
                g, StringAttr::get(g.getContext(), *g.getAliasee()));
        return g ? g.getOperation() : fail(Unplaced::Other);
    }
    if (auto alloca = t.getDefiningOp<cir::AllocaOp>())
        return alloca.getOperation();
    auto arg = dyn_cast<BlockArgument>(t);
    if (!arg) {
        if (auto c = t.getDefiningOp<cir::CastOp>())
            if (c.getKind() == cir::CastKind::int_to_ptr) return fail(Unplaced::Integer);
        return fail(Unplaced::Other);
    }
    if (!inst || !inst->site) return fail(Unplaced::RootParameter);   // a root: nobody passed it
    if (arg.getOwner() != &inst->func.getBody().front()) return fail(Unplaced::Other);
    const unsigned i = arg.getArgNumber();
    cir::CallOp site = inst->site;
    unsigned idx;
    if (getSemanticKind(lookupCallee(site)) == SemanticOpKind::ThreadSpawn) {
        // The entry's parameter i is the i-th argument the spawn passes on.
        idx = spawnFirstArgOperand(site) + i;
    } else {
        idx = i;
    }
    if (idx >= site.getNumOperands()) return fail(Unplaced::Other);
    // Only a spawn hands each thread its own element; through a plain call
    // the per-thread reading does not apply.
    const bool spawn = getSemanticKind(lookupCallee(site)) == SemanticOpKind::ThreadSpawn;
    return resolveRootImpl(site.getOperand(idx), inst->parent, depth + 1,
                           spawn ? site : cir::CallOp(), place, many);
}

Mult ThreadInstanceWalk::multWithinThread(const Instance &inst, bool &known) {
    known = true;
    Mult m = Mult::One;
    for (const Instance *p = &inst; p; p = p->parent) {
        if (p->recurring) m = Mult::Many;
        if (!p->site || !p->parent) break;
        if (getSemanticKind(lookupCallee(p->site)) == SemanticOpKind::ThreadSpawn) break;   // the thread's entry
        bool k;
        cir::CallOp at = p->site;
        const Mult site = siteMult(at.getOperation(), p->parent->func, k);
        if (!k) known = false;
        m = join(m, site);
    }
    return m;
}

bool ThreadInstanceWalk::siteRunsOnce(Operation *op, unsigned depth) {
    auto f = op->getParentOfType<cir::FuncOp>();
    if (!f || depth > 16) return false;
    bool known;
    if (siteMult(op, f, known) != Mult::One || !known) return false;
    if (runsOnce(f)) return true;
    if (f.getSymName() == "main") return true;
    if (instanceCount(f) > 0) return false;     // the walk saw it run more than once
    if (isAddressTaken(f, module_)) return false;
    if (!callersComputed_) {
        callersComputed_ = true;
        ModuleOp module = module_;
        module.walk([&](cir::CallOp call) {
            if (cir::FuncOp callee = lookupCallee(call))
                callers_[callee.getOperation()].push_back(call.getOperation());
        });
    }
    auto it = callers_.find(f.getOperation());
    if (it == callers_.end() || it->second.size() != 1) return false;
    return siteRunsOnce(it->second.front(), depth + 1);
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
        if (auto c = dyn_cast<cir::CastOp>(user)) {
            // The same casts traceToLocalRoot follows backwards: through an
            // integer round trip the entry can no longer be resolved at the
            // spawn, so it must count as an address taken, or the thread is
            // lost on both sides.
            if (c.getKind() != cir::CastKind::bitcast && c.getKind() != cir::CastKind::array_to_ptrdecay)
                return false;
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

// The cap bounds every instance, roots included: an earlier version added
// roots unchecked, so with a small cap the passes gave verdicts over more
// instances than the cap allowed and never said they had given up.
Instance *ThreadInstanceWalk::addIfRoom(Instance inst) {
    if (instances_.size() >= cap_) { capExceeded_ = true; return nullptr; }
    return &addInstance(std::move(inst));
}

// Remarks are held back until the walk is over: past the cap the passes
// print one line and give up, and a page of remarks before it only buries
// that line. One remark per operation, however many instances reach it.
void ThreadInstanceWalk::noteRemark(Operation *at, const llvm::Twine &msg) {
    if (!remarkedAt_.insert(at).second) return;
    pendingRemarks_.emplace_back(at, msg.str());
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
            addIfRoom({mainFn, nextThread_++, Mult::One, Mult::One, nullptr, nullptr, false,
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
                    addIfRoom({f, mainThread, Mult::One, Mult::One, nullptr, nullptr, false,
                               !ctorSpawns});
        }
    }

    // Address-taken functions may run anywhere, any number of times.
    module_.walk([&](cir::FuncOp f) {
        if (f.getBody().empty()) return;
        if (getSemanticKind(f) != SemanticOpKind::Unknown) return;   // shim bodies carry no wtc ops
        if (isAddressTaken(f, module_) && relevant_(f)) {
            if (addIfRoom({f, nextThread_++, Mult::Many, Mult::Many, nullptr, nullptr, true, false}))
                noteRemark(f, "wtc: address of this function is taken; it is treated as callable "
                              "from any thread, any number of times");
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
                noteRemark(call, "wtc: spawn target could not be resolved to a function in this module");
                return;
            }
            if (!relevant_(entry)) return;
            // A site whose multiplicity is a guess (a goto in this function,
            // no loop around the site) makes the child's count unknown rather
            // than many: an earlier version took the guess, so one spawn
            // below a goto in main stood for many threads and every store
            // they made was "repeated".
            // The multiplicity keeps the guess -- a client that must assume
            // the worst, such as the queue analysis, reads Many -- and the
            // instance records that it is one, so a client that would turn
            // Many into a claim can decline to.
            bool known;
            const Mult site = siteMult(call.getOperation(), f, known);
            const Mult m = join(inst.execMult, site);
            const bool certainlyMany = (inst.execMult == Mult::Many && !inst.execUnknown) ||
                                       (known && site == Mult::Many);
            if (Instance *child = addIfRoom({entry, nextThread_++, m, m, call, &inst,
                                             inst.speculative, false}))
                child->execUnknown = !certainlyMany && (!known || inst.execUnknown);
            return;
        }
        if (kind != SemanticOpKind::Unknown) return;           // shim functions: nothing inside
        if (callee.getBody().empty()) return;                  // a declaration
        if (!relevant_(callee)) return;
        bool known;
        const Mult site = siteMult(call.getOperation(), f, known);
        const Mult m = join(inst.execMult, site);
        const bool certainlyMany = (inst.execMult == Mult::Many && !inst.execUnknown) ||
                                   (known && site == Mult::Many);
        if (Instance *child = addIfRoom({callee, inst.thread, inst.threadMult, m, call, &inst,
                                         inst.speculative,
                                         inst.startup && !spawnPrecedes(call.getOperation(), f)})) {
            child->execUnknown = !certainlyMany && (!known || inst.execUnknown);
            // In an unstructured function that can spawn, "no spawn precedes
            // this call" is a guess: the goto may jump over the spawn or
            // back behind it.
            child->startupUnknown = inst.startupUnknown ||
                                    (inst.startup && isUnstructured(f) && canSpawn(f));
        }
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
        addIfRoom({f, nextThread_++, Mult::Many, Mult::Many, nullptr, nullptr, true, false});
    });
}

void ThreadInstanceWalk::run(llvm::function_ref<void(Instance &)> visit,
                             llvm::function_ref<void(cir::FuncOp)> onRecursion) {
    auto step = [&](Instance &inst) {
        // Recursion: the chain already holds this function. The first
        // recurrence is visited and expanded once more, standing for many
        // runs (or, when a spawn closed the cycle, many threads), so that
        // what the recursive call reaches is seen -- an earlier version
        // skipped it and lost every access the recursive call made to
        // objects the outer call did not touch, and a worker that spawned
        // itself through a helper lost the helper's writes. The second
        // recurrence is visited and not expanded, which bounds the walk.
        bool seen = false;
        for (Instance *p = inst.parent; p; p = p->parent)
            if (p->func == inst.func) { seen = true; break; }
        if (seen) {
            onRecursion(inst.func);
            inst.recurring = true;
            if (inst.site && getSemanticKind(lookupCallee(inst.site)) == SemanticOpKind::ThreadSpawn)
                inst.threadMult = inst.execMult = Mult::Many;
            else
                inst.execMult = Mult::Many;
            // Each function's recurrence is expanded once in the whole walk:
            // unrolling every cycle once per path made the walk exponential
            // in the size of a mutually recursive group.
            // Once per function, thread and phase: the walk is breadth-first,
            // so keyed by function alone the one unrolling went to the
            // shallowest recurrence, a start-up call, and a worker's
            // recursive writes were never seen.
            // A recurrence through a spawn starts a new thread each time, so
            // keyed by thread it would unroll forever; once per function is
            // right for it, since the chain of threads is one thing.
            const bool viaSpawn = inst.site && getSemanticKind(lookupCallee(inst.site)) == SemanticOpKind::ThreadSpawn;
            const uint64_t key = viaSpawn ? ~uint64_t(0)
                                          : ((uint64_t(inst.thread) << 2) | (inst.startup ? 2 : 0) |
                                             (inst.startupUnknown ? 1 : 0));
            if (!expandedRecurrence_.insert({inst.func.getOperation(), key}).second) {
                // Visited, not expanded: what this recurrence's calls reach
                // with its own arguments is not in the walk, and may be an
                // object the expanded one did not touch.
                inst.truncated = true;
                visit(inst);
                return;
            }
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

    if (capExceeded_) return;
    for (auto &[at, msg] : pendingRemarks_) {
        at->emitRemark() << msg;
        ++remarks_;
    }
}
