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

// A bounded queue (runtime/bounded_ring.hpp) takes a ticket and then may
// wait for the operation of the previous lap, or for the push of its own
// ticket, to finish with the cell: an operation in progress, but a wait.
// It never allocates: the ring is built with the queue.
Progress QueuePushOp::getProgress() {
    if (getBounded()) return Progress::blocking;
    return segmented(implOf(getImpl())) ? Progress::lock_free : Progress::wait_free;
}
bool QueuePushOp::mayCallAllocator() { return !getBounded(); }
std::string QueuePushOp::getBlockingReason() {
    if (getBounded()) {
        std::string why = "may wait for the pop that last held its cell to finish moving the item out";
        if (getWait()) return "waits while the queue is full, as its wait policy says; and " + why;
        return why;
    }
    switch (implOf(getImpl())) {
    case ::mlir::wtc::Impl::spsc:
    case ::mlir::wtc::Impl::spsc_inline:
        return "allocates a new block when the current one is full: the queue is unbounded";
    default:
        return "allocates a new segment when the current one is full: the queue is unbounded";
    }
}

// The general-purpose pop waits for a push that has reserved its cell. The
// spmc pop claims a published cell with a compare-and-swap that fails only
// when another consumer has claimed it: lock-free.
Progress QueuePopOp::getProgress() {
    // A waiting pop waits for a push by design, whatever the queue and its
    // wait policy: spinning is waiting too.
    if (getWait() || getBounded()) return Progress::blocking;
    switch (implOf(getImpl())) {
    case ::mlir::wtc::Impl::generic:
        return Progress::blocking;
    case ::mlir::wtc::Impl::spmc:
        return Progress::lock_free;
    default:
        return Progress::wait_free;
    }
}
// A pop that returns a shared_ptr allocates it, on the consumer's side,
// from the element in the cell; a pop that moves the element out
// (transfer) or lends it (spsc_inline) allocates nothing, and only the
// queues made of segments (general-purpose, mpsc, spmc) then still free the
// segments they move past.
bool QueuePopOp::mayCallAllocator() {
    if (getBounded()) return !getTransfer();
    const ::mlir::wtc::Impl impl = implOf(getImpl());
    if (getTransfer()) return segmented(impl) || impl == ::mlir::wtc::Impl::spmc;
    return impl != ::mlir::wtc::Impl::spsc_inline;
}
std::string QueuePopOp::getBlockingReason() {
    if (getBounded()) {
        std::string why = "may wait for the push that took its cell to fill it";
        if (getWait())
            return (getTimeout() ? "waits for a push, for the queue to be closed, or for its time limit"
                                 : "waits for a push or for the queue to be closed") +
                   std::string(", as its wait policy says; and ") + why;
        return why;
    }
    if (getWait()) {
        std::string why = getTimeout() ? "waits for a push, for the queue to be closed, or for its "
                                         "time limit, as its wait policy says: spinning, or asleep "
                                         "in the kernel"
                                       : "waits for a push or for the queue to be closed, as its "
                                         "wait policy says: spinning, or asleep in the kernel";
        switch (implOf(getImpl())) {
        case ::mlir::wtc::Impl::generic:
            return why + "; and frees the segments it moves past";
        case ::mlir::wtc::Impl::mpsc:
            return why + "; and frees the segments it moves past, and may allocate to note the "
                         "cells it passes that a push has taken and not filled";
        case ::mlir::wtc::Impl::spmc:
            return why + "; and frees the segments it moves past";
        default:
            return why;
        }
    }
    const bool shares = !getTransfer();
    switch (implOf(getImpl())) {
    case ::mlir::wtc::Impl::generic:
        return std::string("waits for a push that has taken the cell it reads to fill it, and "
                           "frees the segments it moves past") +
               (shares ? ", and allocates the shared_ptr it returns" : "");
    case ::mlir::wtc::Impl::mpsc:
        return std::string("frees the segments it moves past, and may allocate to note the cells "
                           "it passes that a push has taken and not filled") +
               (shares ? ", and allocates the shared_ptr it returns" : "");
    case ::mlir::wtc::Impl::spmc:
        return std::string("frees the segments it moves past") +
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
        return "allocates a new block when the current one is full: the queue is unbounded";
    case ::mlir::wtc::Impl::spmc:
        return "allocates a new segment when the current one is full: the queue is unbounded";
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
