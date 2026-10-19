#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/OpImplementation.h"

using namespace mlir;
using namespace mlir::wtc;

#define GET_OP_CLASSES
#include "WTC/WTCOps.cpp.inc"

namespace {

// A function implements an operation if it carries the shim's annotation
// for it, or a substitute implementation's: wtc_impl_<name>_<operation>.
bool implementsOperation(cir::FuncOp func, StringRef shimAnnotation, StringRef operation) {
    auto annotations = func.getAnnotationsAttr();
    if (!annotations) return false;
    for (Attribute attr : annotations) {
        auto annotation = mlir::dyn_cast<cir::AnnotationAttr>(attr);
        if (!annotation) continue;
        StringRef name = annotation.getName().getValue();
        if (name == shimAnnotation) return true;
        if (name.starts_with("wtc_impl_") && name.ends_with(("_" + operation).str()))
            return true;
    }
    return false;
}

// fallback_impl is optional, but when present it has to be something the
// operation can be lowered to: a cir.func implementing the same operation
// and taking the arguments lowering will pass.
LogicalResult verifyFallbackImpl(Operation *op, FlatSymbolRefAttr impl, StringRef shimAnnotation,
                                 StringRef operation, unsigned expectedArgs,
                                 SymbolTableCollection &symbolTable) {
    if (!impl) return success();
    auto func = symbolTable.lookupNearestSymbolFrom<cir::FuncOp>(op, impl);
    if (!func) return op->emitOpError("fallback_impl ") << impl << " does not name a cir.func";
    if (!implementsOperation(func, shimAnnotation, operation))
        return op->emitOpError("fallback_impl ")
               << impl << " is not annotated " << shimAnnotation << " or wtc_impl_*_" << operation;
    if (func.getNumArguments() != expectedArgs)
        return op->emitOpError("fallback_impl ")
               << impl << " takes " << func.getNumArguments() << " arguments, but lowering passes "
               << expectedArgs;
    return success();
}

// Lowering casts the queue handle back to source_queue_type, so that type
// has to be a pointer to the object the queue type names.
LogicalResult verifySourceQueueType(Operation *op, Type queue, std::optional<Type> source) {
    if (!source) return success();
    auto ptr = mlir::dyn_cast<cir::PointerType>(*source);
    auto queueType = mlir::cast<QueueType>(queue);
    if (!ptr || ptr.getPointee() != queueType.getElementType())
        return op->emitOpError("source_queue_type ")
               << *source << " is not a pointer to " << queueType.getElementType();
    return success();
}

} // namespace

LogicalResult LockOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_mutex_lock", "lock", 1, symbolTable);
}

LogicalResult UnlockOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_mutex_unlock", "unlock", 1,
                              symbolTable);
}

LogicalResult QueuePushOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_push", "push", 2, symbolTable);
}

LogicalResult QueuePopOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    const unsigned expectedArgs = getOut() ? 2 : 1;
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_try_pop", "try_pop",
                              expectedArgs, symbolTable);
}

LogicalResult QueuePushOp::verify() {
    return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
}

LogicalResult QueuePopOp::verify() {
    const bool hasOut = static_cast<bool>(getOut());
    const bool hasValue = static_cast<bool>(getValue());
    if (hasOut == hasValue)
        return emitOpError("needs exactly one of an `out` operand or a result");
    return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
}
