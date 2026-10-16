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
                                         inst.startup && !spawnPrecedes(call.getOperation(), f)}))
            child->execUnknown = !certainlyMany && (!known || inst.execUnknown);
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
        unsigned seen = 0;
        for (Instance *p = inst.parent; p; p = p->parent)
            if (p->func == inst.func) ++seen;
        if (seen) {
            onRecursion(inst.func);
            if (inst.site && getSemanticKind(lookupCallee(inst.site)) == SemanticOpKind::ThreadSpawn)
                inst.threadMult = inst.execMult = Mult::Many;
            else
                inst.execMult = Mult::Many;
            if (seen >= 2) { visit(inst); return; }
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
