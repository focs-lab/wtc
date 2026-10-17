#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseSet.h"

namespace mlir::wtc {

cir::FuncOp lookupCallee(cir::CallOp op) {
    auto calleeAttr = op.getCalleeAttr();
    if (!calleeAttr)
        return nullptr;
    auto module = op->getParentOfType<mlir::ModuleOp>();
    if (!module)
        return nullptr;
    Operation *callee = mlir::SymbolTable::lookupNearestSymbolFrom(module, calleeAttr);
    return mlir::dyn_cast_or_null<cir::FuncOp>(callee);
}

bool hasAnnotation(cir::FuncOp funcOp, StringRef annotation) {
    if (!funcOp)
        return false;
    auto annotations = funcOp.getAnnotationsAttr();
    if (!annotations)
        return false;
    for (auto attr : annotations) {
        auto annotAttr = mlir::dyn_cast<cir::AnnotationAttr>(attr);
        if (!annotAttr)
            continue;
        if (annotAttr.getName().getValue() == annotation)
            return true;
    }
    return false;
}

bool isWTCAnnotatedCall(cir::CallOp op, StringRef annotation) {
    return hasAnnotation(lookupCallee(op), annotation);
}

// Standard entry points that mean what the shim's annotations mean. Only
// names with a settled meaning belong here: pthread_create starts a thread
// in every C program that has one, and a program that defines its own
// function with that name is already in trouble.
//
// libstdc++'s std::thread constructor is a template instantiated in the
// translation unit, and it hands the callable to a bodiless
// std::thread::_M_start_thread. Its entry cannot be resolved -- it travels
// inside a heap-allocated state object -- but recognising the call matters
// anyway: it is where the start-up phase ends, and without it every
// program that uses std::thread looked as if it never started a thread.
static SemanticOpKind kindByName(StringRef name) {
    if (name == "pthread_create" || name == "thrd_create") return SemanticOpKind::ThreadSpawn;
    // DPDK's EAL starts a worker on another lcore: rte_eal_remote_launch(f, arg,
    // worker_id), entry first and its argument next, as the shim's spawn.
    if (name == "rte_eal_remote_launch") return SemanticOpKind::ThreadSpawn;
    if (name.contains("6thread15_M_start_thread")) return SemanticOpKind::ThreadSpawn;
    return SemanticOpKind::Unknown;
}

SemanticOpKind getSemanticKind(cir::FuncOp funcOp) {
    if (!funcOp)
        return SemanticOpKind::Unknown;
    if (SemanticOpKind k = kindByName(funcOp.getSymName()); k != SemanticOpKind::Unknown)
        return k;
    auto annotations = funcOp.getAnnotationsAttr();
    if (!annotations)
        return SemanticOpKind::Unknown;
    for (auto attr : annotations) {
        auto annotAttr = mlir::dyn_cast<cir::AnnotationAttr>(attr);
        // Other attributes may sit in the same array; skip them rather than
        // give up, which is what an earlier version did and which hid every
        // spawn behind one unrelated annotation.
        if (!annotAttr)
            continue;
        StringRef name = annotAttr.getName().getValue();
        if (name == "wtc_thread_spawn")       return SemanticOpKind::ThreadSpawn;
        if (name == "wtc_queue_construction") return SemanticOpKind::QueueConstruction;
        if (name == "wtc_queue_push")         return SemanticOpKind::QueuePush;
        if (name == "wtc_queue_try_pop")      return SemanticOpKind::QueueTryPop;
        if (name == "wtc_mutex_lock")         return SemanticOpKind::MutexLock;
        if (name == "wtc_mutex_unlock")       return SemanticOpKind::MutexUnlock;
    }
    return SemanticOpKind::Unknown;
}

static bool dictHasSret(Attribute dictLike) {
    auto dict = mlir::dyn_cast_or_null<DictionaryAttr>(dictLike);
    return dict && dict.get("llvm.sret");
}

bool hasSretArg(cir::CallOp op, unsigned idx) {
    if (auto argAttrs = op.getArgAttrsAttr()) {
        if (idx < argAttrs.size() && dictHasSret(argAttrs[idx]))
            return true;
    }
    if (auto callee = lookupCallee(op)) {
        if (auto argAttrs = callee.getArgAttrsAttr()) {
            if (idx < argAttrs.size() && dictHasSret(argAttrs[idx]))
                return true;
        }
    }
    return false;
}

unsigned firstRealOperand(cir::CallOp op) {
    return (op.getNumOperands() > 0 && hasSretArg(op, 0)) ? 1u : 0u;
}

Value traceToLocalRoot(Value v) {
    while (true) {
        Operation* def = v.getDefiningOp();
        if (!def)
            return v;
        // Only the casts under which the value still names the same object:
        // a reinterpretation of the pointer, or an array decaying to a
        // pointer to its first element. A pointer made from an integer
        // names nothing the walk can vouch for, whatever the integer was
        // made from, and following it once placed `(S *)(uintptr_t)p + k`
        // on p's object.
        if (auto castOp = mlir::dyn_cast<cir::CastOp>(def)) {
            const cir::CastKind kind = castOp.getKind();
            if (kind != cir::CastKind::bitcast && kind != cir::CastKind::array_to_ptrdecay)
                return v;
            v = castOp.getSrc();
            continue;
        }
        if (auto uccOp = mlir::dyn_cast<mlir::UnrealizedConversionCastOp>(def)) {
            if (uccOp.getNumOperands() != 1)
                return v;
            v = uccOp.getOperand(0);
            continue;
        }
        return v;
    }
}

cir::FuncOp resolveFunctionOperand(Value v) {
    Value root = traceToLocalRoot(v);
    auto getGlobal = root.getDefiningOp<cir::GetGlobalOp>();
    if (!getGlobal)
        return nullptr;
    return mlir::SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(
        getGlobal, getGlobal.getNameAttr());
}

// The stored value if `alloca` is written exactly once, null stores aside,
// by a store in its own block that precedes every load, and nothing else
// touches its address.
static Value singleStoredValue(cir::AllocaOp alloca) {
    cir::StoreOp theStore;
    llvm::SmallVector<cir::LoadOp, 8> loads;
    for (Operation *user : alloca->getUsers()) {
        if (auto st = mlir::dyn_cast<cir::StoreOp>(user)) {
            if (st.getAddr() != alloca.getResult())
                return nullptr;          // the slot's address is the stored value
            // `p = NULL` is no second value: a null reaches no field access,
            // and `T *p = NULL; ... p = &x;` is the commonest shape of a
            // pointer local.
            if (auto c = st.getValue().getDefiningOp<cir::ConstantOp>())
                if (auto cp = mlir::dyn_cast<cir::ConstPtrAttr>(c.getValue()))
                    if (cp.getValue().getValue().isZero()) continue;
            if (theStore)
                return nullptr;          // two stores: not a single assignment
            theStore = st;
            continue;
        }
        if (auto ld = mlir::dyn_cast<cir::LoadOp>(user)) {
            loads.push_back(ld);
            continue;
        }
        return nullptr;                  // address escapes into something else
    }
    if (!theStore || theStore->getBlock() != alloca->getBlock())
        return nullptr;
    Block *block = alloca->getBlock();
    for (cir::LoadOp ld : loads) {
        Operation *anc = block->findAncestorOpInBlock(*ld.getOperation());
        if (!anc || !theStore->isBeforeInBlock(anc))
            return nullptr;              // a load the store does not dominate
    }
    return theStore.getValue();
}

Value traceToRoot(Value v) {
    while (true) {
        Value local = traceToLocalRoot(v);
        auto load = local.getDefiningOp<cir::LoadOp>();
        if (!load)
            return local;
        auto alloca = load.getAddr().getDefiningOp<cir::AllocaOp>();
        if (!alloca)
            return local;
        Value stored = singleStoredValue(alloca);
        if (!stored)
            return local;
        v = stored;
    }
}


// A stack slot is a different object in every invocation, so two threads
// running the same function do not share it and cannot falsely share its
// fields. Running on real compiler output, this was the whole of the
// pass's noise: libstdc++ locals such as allocated_ptr's __guard_ptr,
// reported because the function they live in is reachable from many
// threads. They are only interesting if the address is published, and
// even then two invocations are two objects, so an escaping slot can
// reach "possible" and never "definite".
bool addressEscapes(cir::AllocaOp slot) {
    llvm::SmallVector<Value, 8> work{slot.getResult()};
    llvm::DenseSet<Operation *> seen;
    unsigned guard = 0;
    while (!work.empty() && guard++ < 512) {
        Value v = work.pop_back_val();
        for (Operation *u : v.getUsers()) {
            // A store is judged per operand: the same store may hold
            // the slot's address as its destination and, through another
            // derived value, as the value stored, and only the second
            // publishes it.
            if (auto st = dyn_cast<cir::StoreOp>(u)) {
                if (st.getValue() == v) return true;
                continue;
            }
            if (!seen.insert(u).second) continue;
            if (isa<cir::LoadOp>(u)) continue;                  // reads the slot
            if (isa<cir::CastOp, cir::GetMemberOp, cir::GetElementOp, cir::PtrStrideOp>(u)) {
                work.push_back(u->getResult(0));
                continue;
            }
            return true;    // a call, a global initialiser, anything else
        }
    }
    // Out of budget with work left: assume the worst. With the work
    // done, the address went nowhere. An earlier version returned true
    // on both exits, so no slot was ever private and the summary's
    // "on stack slots that never escape" was always zero.
    return !work.empty();
}

unsigned spawnEntryOperand(cir::CallOp op) {
    cir::FuncOp callee = lookupCallee(op);
    // pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    //                void *(*start)(void *), void *arg)
    if (callee && callee.getSymName() == "pthread_create")
        return firstRealOperand(op) + 2;
    // thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
    if (callee && callee.getSymName() == "thrd_create")
        return firstRealOperand(op) + 1;
    return firstRealOperand(op);
}

unsigned spawnFirstArgOperand(cir::CallOp op) {
    return spawnEntryOperand(op) + 1;
}

bool isAllocator(cir::FuncOp f) {
    if (!f) return false;
    const StringRef n = f.getSymName();
    if (n == "malloc" || n == "calloc" || n == "aligned_alloc" || n == "memalign" ||
        n == "posix_memalign" || n == "_Znwm" || n == "_Znam" ||
        n == "_ZnwmSt11align_val_t" || n == "_ZnamSt11align_val_t")
        return true;
    // DPDK's EAL heap, rte_malloc(type, size, align) and its zeroing, counting
    // and per-socket forms, and the ring and stack constructors, which hand
    // back a fresh object that exists nowhere else: rte_ring_create(name,
    // count, socket, flags), rte_ring_create_elem(name, esize, count,
    // socket, flags) and rte_stack_create(name, count, socket, flags). The
    // memzone behind them is reserved cache-line aligned, which the pass
    // does not read; the heap-align option stands in for it.
    return n == "rte_malloc" || n == "rte_zmalloc" || n == "rte_calloc" ||
           n == "rte_malloc_socket" || n == "rte_zmalloc_socket" || n == "rte_calloc_socket" ||
           n == "rte_ring_create" || n == "rte_ring_create_elem" || n == "rte_stack_create";
}

static cir::CallOp allocationBehindImpl(cir::CallOp call, unsigned depth) {
    cir::FuncOp callee = lookupCallee(call);
    if (!callee) return cir::CallOp();
    if (isAllocator(callee)) return call;
    if (callee.getBody().empty() || depth > 4) return cir::CallOp();
    cir::CallOp site;
    bool bad = false;
    callee.walk([&](cir::ReturnOp ret) {
        if (bad) return;
        if (ret.getNumOperands() != 1) { bad = true; return; }
        Value r = traceToRoot(ret.getOperand(0));
        // A null handed back on the failure path reaches no field access.
        if (auto c = r.getDefiningOp<cir::ConstantOp>())
            if (auto cp = dyn_cast<cir::ConstPtrAttr>(c.getValue()))
                if (cp.getValue().getValue().isZero()) return;
        cir::CallOp here;
        if (auto c = r.getDefiningOp<cir::CallOp>()) here = allocationBehindImpl(c, depth + 1);
        if (!here || (site && site != here)) bad = true;
        else site = here;
    });
    return bad ? cir::CallOp() : site;
}

cir::CallOp allocationBehind(cir::CallOp call) { return allocationBehindImpl(call, 0); }

std::optional<uint64_t> allocationAlignment(cir::CallOp call) {
    call = allocationBehind(call);
    cir::FuncOp callee = call ? lookupCallee(call) : cir::FuncOp();
    if (!callee) return std::nullopt;
    const StringRef n = callee.getSymName();
    unsigned idx;
    if (n == "aligned_alloc" || n == "memalign") idx = firstRealOperand(call);
    else if (n == "posix_memalign") idx = firstRealOperand(call) + 1;
    else if (n == "_ZnwmSt11align_val_t" || n == "_ZnamSt11align_val_t") idx = firstRealOperand(call) + 1;
    // rte_malloc(type, size, align), rte_calloc(type, num, size, align); an
    // align of 0 means the EAL's default, which is not read here.
    else if (n == "rte_malloc" || n == "rte_zmalloc") idx = firstRealOperand(call) + 2;
    else if (n == "rte_calloc") idx = firstRealOperand(call) + 3;
    else if (n == "rte_malloc_socket" || n == "rte_zmalloc_socket") idx = firstRealOperand(call) + 2;
    else if (n == "rte_calloc_socket") idx = firstRealOperand(call) + 3;
    else return std::nullopt;
    if (idx >= call.getNumOperands()) return std::nullopt;
    if (auto c = call.getOperand(idx).getDefiningOp<cir::ConstantOp>())
        if (auto a = dyn_cast<cir::IntAttr>(c.getValue()))
            if (a.getValue().getActiveBits() <= 32 && a.getValue().getZExtValue() > 0)
                return a.getValue().getZExtValue();
    return std::nullopt;
}

void memoryAccesses(Operation *op, llvm::SmallVectorImpl<MemoryAccess> &out) {
    auto pointee = [](Value ptr) -> Type {
        if (auto pt = dyn_cast<cir::PointerType>(ptr.getType())) return pt.getPointee();
        return Type();
    };
    // A libc memory call touches `len` bytes: an array of that many bytes
    // when the length is a constant, else the pointee, which is at least
    // what the call was written over.
    // Behind the void* the call takes sits the pointer the program wrote,
    // whose pointee is the least the call touches; failing that, one byte.
    auto lengthType = [&](Value len, Value ptr) -> Type {
        if (auto c = len.getDefiningOp<cir::ConstantOp>())
            if (auto n = dyn_cast<cir::IntAttr>(c.getValue()))
                if (n.getValue().getActiveBits() <= 32 && n.getValue().getZExtValue() > 0)
                    return cir::ArrayType::get(op->getContext(),
                                               cir::IntType::get(op->getContext(), 8, false),
                                               n.getValue().getZExtValue());
        Type t = pointee(traceToLocalRoot(ptr));
        if (!t || isa<cir::VoidType>(t) || isa<cir::FuncType>(t))
            t = cir::IntType::get(op->getContext(), 8, false);
        return t;
    };
    if (auto ld = dyn_cast<cir::LoadOp>(op)) {
        out.push_back({ld.getAddr(), ld.getResult().getType(), false});
    } else if (auto st = dyn_cast<cir::StoreOp>(op)) {
        out.push_back({st.getAddr(), st.getValue().getType(), true});
    } else if (auto rmw = dyn_cast<cir::AtomicFetchOp>(op)) {
        out.push_back({rmw.getPtr(), rmw.getVal().getType(), true});
    } else if (auto x = dyn_cast<cir::AtomicXchgOp>(op)) {
        out.push_back({x.getPtr(), x.getVal().getType(), true});
    } else if (auto cx = dyn_cast<cir::AtomicCmpXchgOp>(op)) {
        out.push_back({cx.getPtr(), cx.getDesired().getType(), true});
    } else if (auto ts = dyn_cast<cir::AtomicTestAndSetOp>(op)) {
        out.push_back({ts.getPtr(), pointee(ts.getPtr()), true});
    } else if (auto cl = dyn_cast<cir::AtomicClearOp>(op)) {
        out.push_back({cl.getPtr(), pointee(cl.getPtr()), true});
    } else if (auto sb = dyn_cast<cir::SetBitfieldOp>(op)) {
        // The whole storage unit is rewritten, whatever the field's width.
        out.push_back({sb.getAddr(), pointee(sb.getAddr()), true});
    } else if (auto gb = dyn_cast<cir::GetBitfieldOp>(op)) {
        out.push_back({gb.getAddr(), pointee(gb.getAddr()), false});
    } else if (auto cp = dyn_cast<cir::CopyOp>(op)) {
        out.push_back({cp.getSrc(), pointee(cp.getSrc()), false});
        out.push_back({cp.getDst(), pointee(cp.getDst()), true});
    } else if (auto ms = dyn_cast<cir::MemSetOp>(op)) {
        out.push_back({ms.getDst(), lengthType(ms.getLen(), ms.getDst()), true});
    } else if (auto mc = dyn_cast<cir::MemCpyOp>(op)) {
        out.push_back({mc.getSrc(), lengthType(mc.getLen(), mc.getSrc()), false});
        out.push_back({mc.getDst(), lengthType(mc.getLen(), mc.getDst()), true});
    } else if (auto mm = dyn_cast<cir::MemMoveOp>(op)) {
        out.push_back({mm.getSrc(), lengthType(mm.getLen(), mm.getSrc()), false});
        out.push_back({mm.getDst(), lengthType(mm.getLen(), mm.getDst()), true});
    }
}
}
