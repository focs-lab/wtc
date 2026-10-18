#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/SymbolTable.h"

#define GET_OP_CLASSES
#include "WTC/WTCOps.cpp.inc"

using namespace mlir;
using namespace mlir::wtc;

namespace {

// A function implements an operation if it carries the shim's annotation
// for it, or a substitute implementation's: wtc_impl_<name>_<operation>.
bool implementsOperation(cir::FuncOp func, StringRef shimAnnotation, StringRef operation) {
    auto annotations = func.getAnnotationsAttr();
    if (!annotations)
        return false;
    for (Attribute attr : annotations) {
        auto annotation = mlir::dyn_cast<cir::AnnotationAttr>(attr);
        if (!annotation)
            continue;
        StringRef name = annotation.getName().getValue();
        if (name == shimAnnotation)
            return true;
        if (name.starts_with("wtc_impl_") && name.ends_with(("_" + operation).str()))
            return true;
    }
    return false;
}

// fallback_impl is optional, but when present it has to be something the op
// can actually be lowered to: a cir.func implementing the same operation and
// taking the arguments the lowering will pass.
LogicalResult verifyFallbackImpl(Operation *op, FlatSymbolRefAttr impl,
                                 StringRef shimAnnotation, StringRef operation,
                                 unsigned expectedArgs,
                                 SymbolTableCollection &symbolTable) {
    if (!impl)
        return success();

    auto func = symbolTable.lookupNearestSymbolFrom<cir::FuncOp>(op, impl);
    if (!func)
        return op->emitOpError("fallback_impl ") << impl << " does not name a cir.func";

    if (!implementsOperation(func, shimAnnotation, operation))
        return op->emitOpError("fallback_impl ") << impl << " is not annotated "
            << shimAnnotation << " or wtc_impl_*_" << operation;

    if (func.getNumArguments() != expectedArgs)
        return op->emitOpError("fallback_impl ") << impl << " takes "
            << func.getNumArguments() << " arguments, but lowering passes " << expectedArgs;

    return success();
}

// The value pushed is passed by reference, and a pop that returns a pointer
// returns one to the element; either way the pointee has to be the queue's
// element type. Anything else -- the shared_ptr try_pop returns -- is
// accepted, since nothing here knows its layout.
LogicalResult verifyPointsToElement(Operation *op, Type type, QueueType queueType,
                                    StringRef what) {
    auto ptrType = mlir::dyn_cast<cir::PointerType>(type);
    if (!ptrType || ptrType.getPointee() == queueType.getElementType())
        return success();
    return op->emitOpError() << what << " points to " << ptrType.getPointee()
        << ", but the queue holds " << queueType.getElementType();
}

}

LogicalResult LockOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_mutex_lock", "lock", 1, symbolTable);
}

LogicalResult UnlockOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_mutex_unlock", "unlock", 1, symbolTable);
}

LogicalResult QueuePushOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_push", "push", 2, symbolTable);
}

LogicalResult QueuePopOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    unsigned expectedArgs = getResultSlot() ? 2 : 1;
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_try_pop", "try_pop", expectedArgs, symbolTable);
}

LogicalResult QueuePushOp::verify() {
    return verifyPointsToElement(*this, getValue().getType(),
                                 mlir::cast<QueueType>(getQueue().getType()), "the pushed value");
}

LogicalResult QueuePopOp::verify() {
    if (static_cast<bool>(getResultSlot()) == static_cast<bool>(getValue()))
        return emitOpError("must either return the popped value or write it through "
                           "result_slot, not ") << (getValue() ? "both" : "neither");
    if (Value value = getValue())
        return verifyPointsToElement(*this, value.getType(),
                                     mlir::cast<QueueType>(getQueue().getType()), "the popped value");
    return success();
}
