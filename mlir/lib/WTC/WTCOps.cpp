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
    return verifyFallbackImpl(*this, getFallbackImplAttr(), "wtc_queue_push", "push", 2, symbolTable);
}

LogicalResult QueuePopOp::verifySymbolUses(SymbolTableCollection &symbolTable) {
    const unsigned expectedArgs = getOut() ? 2 : 1;
    return verifyFallbackImpl(*this, getFallbackImplAttr(),
                              getTransfer() ? "wtc_queue_try_pop_into" : "wtc_queue_try_pop",
                              "try_pop", expectedArgs, symbolTable);
}

LogicalResult QueuePushOp::verify() {
    return verifySourceQueueType(*this, getQueue().getType(), getSourceQueueType());
}

LogicalResult QueuePopOp::verify() {
    const bool hasOut = static_cast<bool>(getOut());
    const bool hasValue = static_cast<bool>(getValue());
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
// Progress (decisions/0004). One table: what each operation's algorithm can
// wait for, and whether it may call the allocator, under the implementation
// recorded on it. No #wtc.impl means the general-purpose implementation.

namespace {

::mlir::wtc::Impl implOf(std::optional<::mlir::wtc::Impl> impl) {
    return impl.value_or(::mlir::wtc::Impl::generic);
}

} // namespace

Progress LockOp::getProgress() { return Progress::blocking; }
bool LockOp::mayCallAllocator() { return false; }
std::string LockOp::getBlockingReason() { return "takes a mutex"; }

Progress UnlockOp::getProgress() { return Progress::blocking; }
bool UnlockOp::mayCallAllocator() { return false; }
std::string UnlockOp::getBlockingReason() {
    return "releases a mutex, which is half of a blocking protocol";
}

Progress LockBatchBeginOp::getProgress() { return Progress::blocking; }
bool LockBatchBeginOp::mayCallAllocator() { return false; }
std::string LockBatchBeginOp::getBlockingReason() { return "takes a mutex"; }

Progress LockBatchStepOp::getProgress() { return Progress::blocking; }
bool LockBatchStepOp::mayCallAllocator() { return false; }
std::string LockBatchStepOp::getBlockingReason() {
    return "lets a mutex go and takes it again every WTC_LOCK_BATCH_K sections";
}

Progress LockBatchEndOp::getProgress() { return Progress::blocking; }
bool LockBatchEndOp::mayCallAllocator() { return false; }
std::string LockBatchEndOp::getBlockingReason() {
    return "releases a mutex, which is half of a blocking protocol";
}

// The segmented queues (general-purpose and mpsc) retry a push whose cell a
// pop skipped, or whose segment another push is linking: lock-free.
static bool segmented(::mlir::wtc::Impl impl) {
    return impl == ::mlir::wtc::Impl::generic || impl == ::mlir::wtc::Impl::mpsc;
}

Progress QueuePushOp::getProgress() {
    return segmented(implOf(getImpl())) ? Progress::lock_free : Progress::wait_free;
}
bool QueuePushOp::mayCallAllocator() { return true; }
std::string QueuePushOp::getBlockingReason() {
    switch (implOf(getImpl())) {
    case ::mlir::wtc::Impl::spsc:
    case ::mlir::wtc::Impl::spsc_inline:
    case ::mlir::wtc::Impl::spmc:
        return "allocates a new block when the current one is full: the queue is unbounded";
    default:
        return "allocates a new segment when the current one is full: the queue is unbounded";
    }
}

Progress QueuePopOp::getProgress() {
    const ::mlir::wtc::Impl impl = implOf(getImpl());
    return impl == ::mlir::wtc::Impl::generic || impl == ::mlir::wtc::Impl::spmc ? Progress::blocking
                                                                                 : Progress::wait_free;
}
// A pop that returns a shared_ptr allocates it, on the consumer's side,
// from the element in the cell; a pop that moves the element out
// (transfer) or lends it (spsc_inline) allocates nothing, and only the
// segmented queues' pops then still free the segments they move past.
bool QueuePopOp::mayCallAllocator() {
    const ::mlir::wtc::Impl impl = implOf(getImpl());
    if (getTransfer()) return segmented(impl);
    return impl != ::mlir::wtc::Impl::spsc_inline;
}
std::string QueuePopOp::getBlockingReason() {
    const bool shares = !getTransfer();
    switch (implOf(getImpl())) {
    case ::mlir::wtc::Impl::generic:
        return std::string("waits for a push that has taken the cell it reads to fill it, and "
                           "frees the segments it moves past") +
               (shares ? ", and allocates the shared_ptr it returns" : "");
    case ::mlir::wtc::Impl::mpsc:
        return std::string("frees the segments it moves past") +
               (shares ? ", and allocates the shared_ptr it returns" : "");
    case ::mlir::wtc::Impl::spmc:
        return std::string("waits for another consumer's pop under the consumers' spin lock") +
               (shares ? ", and allocates the shared_ptr it returns" : "");
    case ::mlir::wtc::Impl::spsc:
        return shares ? "allocates the shared_ptr it returns" : "";
    default:
        return "";
    }
}

Progress QueueBatchBeginOp::getProgress() { return Progress::wait_free; }
bool QueueBatchBeginOp::mayCallAllocator() { return false; }
std::string QueueBatchBeginOp::getBlockingReason() { return ""; }

Progress QueuePushDeferredOp::getProgress() {
    return segmented(implOf(getImpl())) ? Progress::lock_free : Progress::wait_free;
}
bool QueuePushDeferredOp::mayCallAllocator() { return true; }
std::string QueuePushDeferredOp::getBlockingReason() {
    switch (implOf(getImpl())) {
    case ::mlir::wtc::Impl::spsc:
    case ::mlir::wtc::Impl::spsc_inline:
    case ::mlir::wtc::Impl::spmc:
        return "allocates a new block when the current one is full: the queue is unbounded";
    default:
        return "allocates a new segment when it publishes into a full one: the queue is "
               "unbounded";
    }
}

Progress QueueBatchEndOp::getProgress() {
    return segmented(implOf(getImpl())) ? Progress::lock_free : Progress::wait_free;
}
bool QueueBatchEndOp::mayCallAllocator() { return segmented(implOf(getImpl())); }
std::string QueueBatchEndOp::getBlockingReason() {
    if (segmented(implOf(getImpl())))
        return "allocates a new segment when it publishes the run into a full one";
    return "";
}
