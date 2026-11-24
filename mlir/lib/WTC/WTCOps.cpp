#include "WTC/WTCOps.h"
#include "WTC/ProgressTable.h"

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

LogicalResult LockBatchBeginOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_mutex_batch_begin", "batch_begin", 2,
                              symbolTable);
}
LogicalResult LockBatchStepOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_mutex_batch_step", "batch_step", 2,
                              symbolTable);
}
LogicalResult LockBatchEndOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_mutex_batch_end", "batch_end", 2,
                              symbolTable);
}

LogicalResult QueuePushOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    if (getBatch())
        return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_push_batch", "push_batch",
                                  2 + getMore().size(), symbolTable);
    if (getWait())
        return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_push_wait", "push_wait", 2, symbolTable);
    if (getOverrun())
        return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_push_overrun", "push_overrun", 2,
                                  symbolTable);
    if (getPushed())
        return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_try_push", "try_push", 2, symbolTable);
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_push", "push", 2, symbolTable);
}

LogicalResult QueuePopOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    const unsigned expectedArgs = 1 + getArgs().size();
    if (getBatch())
        return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_pop_batch", "pop_batch", expectedArgs,
                                  symbolTable);
    if (getWait())
        return verifyFallbackImpl(*this, getFallbackImplAttr(),
                                  getTimeout() ? "wtc_queue_pop_wait_for" : "wtc_queue_pop_wait",
                                  getTimeout() ? "pop_wait_for" : "pop_wait", expectedArgs, symbolTable);
    return verifyFallbackImpl(*this, getFallbackImplAttr(),
                              getTransfer() ? "wtc_queue_try_pop_into" : "wtc_queue_try_pop",
                              "try_pop", expectedArgs, symbolTable);
}

LogicalResult QueuePushOp::verify() {
    if (getBatch()) {
        if (!getBounded() || !getWait() || getOverrun() || !getPushed())
            return emitOpError("a batch waits while a bounded queue is full and answers how many it appended");
        return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
    }
    if (!getMore().empty())
        return emitOpError("takes operands after the value only as a batch");
    const int forms = (getWait() ? 1 : 0) + (getOverrun() ? 1 : 0) + (getPushed() ? 1 : 0);
    if (forms > 1)
        return emitOpError("is at most one of a waiting push, an overrunning push and a try_push");
    if (forms && !getBounded())
        return emitOpError("waits, overruns or may decline only on a bounded queue");
    return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
}

LogicalResult QueuePopOp::verify() {
    const bool hasOut = static_cast<bool>(getOut());
    const bool hasValue = static_cast<bool>(getValue());
    if (getBatch() && (!getWait() || !getTransfer() || !getBounded()))
        return emitOpError("a batch is a waiting transfer from a bounded queue");
    if (getArgs().size() > 2 && !getBatch())
        return emitOpError("takes at most an `out` operand and a `timeout`");
    if (getTimeout() && !getWait())
        return emitOpError("only a waiting pop takes a `timeout`");
    if (getWait() && !getTransfer())
        return emitOpError("a waiting pop moves the element into its `out` operand: it must be a transfer");
    if (getTransfer()) {
        if (!hasOut) return emitOpError("a transfer pop moves the element into its `out` operand");
    } else if (hasOut == hasValue) {
        return emitOpError("needs exactly one of an `out` operand or a result");
    }
    return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
}

LogicalResult QueueBatchBeginOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_batch_begin", "batch_begin", 2,
                              symbolTable);
}
LogicalResult QueuePushDeferredOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_push_deferred", "push_deferred",
                              3, symbolTable);
}
LogicalResult QueueBatchEndOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_batch_end", "batch_end", 2,
                              symbolTable);
}
LogicalResult QueueBatchBeginOp::verify() {
    return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
}
LogicalResult QueuePushDeferredOp::verify() {
    return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
}
LogicalResult QueueBatchEndOp::verify() {
    return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
}

// ---------------------------------------------------------------------------
// Progress (decisions/0004, docs/progress.md). Every operation answers from
// one table (WTC/ProgressTable.h), by its form, the implementation recorded
// on it and the counts it is asked under. No #wtc.impl means the
// general-purpose implementation.

namespace {

::mlir::wtc::Impl implOf(std::optional<::mlir::wtc::Impl> impl) {
    return impl.value_or(::mlir::wtc::Impl::generic);
}

} // namespace

ProgressSpec DelegateOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::delegate, ::mlir::wtc::Impl::generic, counts);
}

// `section` must be the outlined section: a cir.func with a body, taking
// the environment and returning the exit.
LogicalResult DelegateOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    auto func = symbolTable.lookupNearestSymbolFrom<cir::FuncOp>(*this, getSectionAttr());
    if (!func) return emitOpError("section ") << getSectionAttr() << " does not name a cir.func";
    if (func.getBody().empty()) return emitOpError("section ") << getSectionAttr() << " has no body";
    auto type = func.getFunctionType();
    if (type.getNumInputs() != 1 || type.getInput(0) != getEnv().getType())
        return emitOpError("section ") << getSectionAttr() << " does not take the environment alone";
    if (type.getReturnType() != getExit().getType())
        return emitOpError("section ") << getSectionAttr() << " does not return the exit's type";
    return success();
}

ProgressSpec LockOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::lock, ::mlir::wtc::Impl::generic, counts);
}
ProgressSpec UnlockOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::unlock, ::mlir::wtc::Impl::generic, counts);
}
ProgressSpec LockBatchBeginOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::lock_batch_begin, ::mlir::wtc::Impl::generic, counts);
}
ProgressSpec LockBatchStepOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::lock_batch_step, ::mlir::wtc::Impl::generic, counts);
}
ProgressSpec LockBatchEndOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::lock_batch_end, ::mlir::wtc::Impl::generic, counts);
}

OpForm QueuePushOp::getForm() {
    if (!getBounded()) return OpForm::push;
    if (getBatch()) return OpForm::bounded_push_batch;
    if (getWait()) return OpForm::bounded_push_wait;
    if (getOverrun()) return OpForm::bounded_push_overrun;
    return OpForm::bounded_try_push;
}
ProgressSpec QueuePushOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(getForm(), implOf(getImpl()), counts);
}

OpForm QueuePopOp::getForm() {
    if (getBounded()) {
        if (getBatch()) return OpForm::bounded_pop_batch;
        if (getWait()) return getTimeout() ? OpForm::bounded_pop_wait_for : OpForm::bounded_pop_wait;
        return getTransfer() ? OpForm::bounded_pop_transfer : OpForm::bounded_pop_shared;
    }
    if (getWait()) return getTimeout() ? OpForm::pop_wait_for : OpForm::pop_wait;
    return getTransfer() ? OpForm::pop_transfer : OpForm::pop_shared;
}
ProgressSpec QueuePopOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(getForm(), implOf(getImpl()), counts);
}

ProgressSpec QueueBatchBeginOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::batch_begin, implOf(getImpl()), counts);
}
ProgressSpec QueuePushDeferredOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::push_deferred, implOf(getImpl()), counts);
}
ProgressSpec QueueBatchEndOp::getProgressSpec(const ThreadCounts &counts) {
    return progressSpecFor(OpForm::batch_end, implOf(getImpl()), counts);
}
