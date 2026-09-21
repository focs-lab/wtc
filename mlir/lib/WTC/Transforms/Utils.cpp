#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "mlir/IR/BuiltinOps.h"

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

SemanticOpKind getSemanticKind(cir::FuncOp funcOp) {
    if (!funcOp)
        return SemanticOpKind::Unknown;
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
        if (auto castOp = mlir::dyn_cast<cir::CastOp>(def)) {
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

// The stored value if `alloca` is written exactly once, by a store in its
// own block that precedes every load, and nothing else touches its address.
static Value singleStoredValue(cir::AllocaOp alloca) {
    cir::StoreOp theStore;
    llvm::SmallVector<cir::LoadOp, 8> loads;
    for (Operation *user : alloca->getUsers()) {
        if (auto st = mlir::dyn_cast<cir::StoreOp>(user)) {
            if (st.getAddr() != alloca.getResult())
                return nullptr;          // the slot's address is the stored value
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

}
