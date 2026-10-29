#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include "llvm/ADT/StringRef.h"

// Lifts calls into the shim library up to wtc dialect operations. A call is
// recognised by the annotation on its callee and nothing else. Operands are
// read through the calling convention: a callee returning a non-trivial
// object receives a hidden first pointer argument, so "the queue" is not
// always operand zero and "the result" is not always a result. The earlier
// version read fixed indices and crashed on the first real try_pop.

namespace {

struct CirToWTCConversionTarget : public ConversionTarget {
    CirToWTCConversionTarget(MLIRContext &ctx) : ConversionTarget(ctx) {
        addLegalDialect<WTCDialect>();
        addLegalDialect<cir::CIRDialect>();
        addLegalOp<mlir::UnrealizedConversionCastOp>();

        addDynamicallyLegalOp<cir::CallOp>([](cir::CallOp op) {
            switch (getSemanticKind(lookupCallee(op))) {
            case SemanticOpKind::MutexLock:
            case SemanticOpKind::MutexUnlock:
            case SemanticOpKind::MutexBatchBegin:
            case SemanticOpKind::MutexBatchStep:
            case SemanticOpKind::MutexBatchEnd:
            case SemanticOpKind::QueuePush:
            case SemanticOpKind::QueueTryPop:
            case SemanticOpKind::QueueTryPopInto:
            case SemanticOpKind::QueuePopWait:
            case SemanticOpKind::QueuePopWaitFor:
            case SemanticOpKind::QueueBatchBegin:
            case SemanticOpKind::QueuePushDeferred:
            case SemanticOpKind::QueueBatchEnd:
                return false;
            default:
                return true;
            }
        });
    }
};

Value castTo(ConversionPatternRewriter &rewriter, Location loc, Type to, Value v) {
    if (v.getType() == to)
        return v;
    return mlir::UnrealizedConversionCastOp::create(rewriter, loc, to, v).getResult(0);
}

FlatSymbolRefAttr fallbackOf(cir::CallOp op, MLIRContext *ctx) {
    return FlatSymbolRefAttr::get(ctx, op.getCalleeAttr().getRootReference().getValue());
}

struct LiftMutexCallPattern : public OpConversionPattern<cir::CallOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(cir::CallOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        const SemanticOpKind kind = getSemanticKind(lookupCallee(op));
        const bool batchOp = kind == SemanticOpKind::MutexBatchBegin ||
                             kind == SemanticOpKind::MutexBatchStep || kind == SemanticOpKind::MutexBatchEnd;
        if (kind != SemanticOpKind::MutexLock && kind != SemanticOpKind::MutexUnlock && !batchOp)
            return failure();

        const unsigned k = firstRealOperand(op);
        const unsigned want = batchOp ? 2 : 1;
        if (op.getNumOperands() != k + want)
            return op.emitError(batchOp ? "wtc: a mutex batch operation takes the mutex and the batch; "
                                          "this call has "
                                        : "wtc: a mutex operation takes exactly the mutex; this call has ")
                   << op.getNumOperands() << " operand(s)";

        Value mutex = castTo(rewriter, op.getLoc(), MutexType::get(rewriter.getContext()),
                             adaptor.getOperands()[k]);
        auto sourceType = TypeAttr::get(op.getOperand(k).getType());
        auto argAttrs = op.getArgAttrsAttr();

        if (batchOp) {
            Value batch = adaptor.getOperands()[k + 1];
            auto fallback = fallbackOf(op, getContext());
            if (kind == SemanticOpKind::MutexBatchBegin)
                rewriter.replaceOpWithNewOp<LockBatchBeginOp>(op, mutex, batch, fallback, sourceType, argAttrs);
            else if (kind == SemanticOpKind::MutexBatchStep)
                rewriter.replaceOpWithNewOp<LockBatchStepOp>(op, mutex, batch, fallback, sourceType, argAttrs);
            else
                rewriter.replaceOpWithNewOp<LockBatchEndOp>(op, mutex, batch, fallback, sourceType, argAttrs);
            return success();
        }
        if (kind == SemanticOpKind::MutexLock)
            rewriter.replaceOpWithNewOp<LockOp>(op, mutex, fallbackOf(op, getContext()), sourceType, argAttrs);
        else
            rewriter.replaceOpWithNewOp<UnlockOp>(op, mutex, fallbackOf(op, getContext()), sourceType, argAttrs);
        return success();
    }
};

struct LiftQueueCallPattern : public OpConversionPattern<cir::CallOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(cir::CallOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        const SemanticOpKind kind = getSemanticKind(lookupCallee(op));
        const bool batchOp = kind == SemanticOpKind::QueueBatchBegin ||
                             kind == SemanticOpKind::QueuePushDeferred ||
                             kind == SemanticOpKind::QueueBatchEnd;
        const bool waitOp = kind == SemanticOpKind::QueuePopWait || kind == SemanticOpKind::QueuePopWaitFor;
        if (kind != SemanticOpKind::QueuePush && kind != SemanticOpKind::QueueTryPop &&
            kind != SemanticOpKind::QueueTryPopInto && !waitOp && !batchOp)
            return failure();

        const unsigned k = firstRealOperand(op);
        auto queuePtrTy = mlir::dyn_cast<cir::PointerType>(op.getOperand(k).getType());
        if (!queuePtrTy)
            return op.emitError("wtc: the queue operand is not a pointer");

        // The queue type's parameter is the CIR type of the queue object
        // itself. Nothing downstream needs the element type, and recovering
        // it meant hard-coding the layout of std::shared_ptr.
        auto queueType = QueueType::get(rewriter.getContext(), queuePtrTy.getPointee());
        Value queue = castTo(rewriter, op.getLoc(), queueType, adaptor.getOperands()[k]);
        auto sourceType = TypeAttr::get(op.getOperand(k).getType());
        auto argAttrs = op.getArgAttrsAttr();
        auto fallback = fallbackOf(op, getContext());

        if (batchOp) {
            const unsigned want = kind == SemanticOpKind::QueuePushDeferred ? 3 : 2;
            if (op.getNumOperands() != k + want)
                return op.emitError("wtc: a batch operation takes the queue, the batch")
                       << (want == 3 ? " and one value" : "") << "; this call has "
                       << op.getNumOperands() << " operand(s)";
            Value batch = adaptor.getOperands()[k + 1];
            if (kind == SemanticOpKind::QueueBatchBegin)
                rewriter.replaceOpWithNewOp<QueueBatchBeginOp>(op, queue, batch, fallback, sourceType,
                                                               argAttrs, TopologyAttr(), IntegerAttr(),
                                                               ImplAttr());
            else if (kind == SemanticOpKind::QueueBatchEnd)
                rewriter.replaceOpWithNewOp<QueueBatchEndOp>(op, queue, batch, fallback, sourceType,
                                                             argAttrs, TopologyAttr(), IntegerAttr(),
                                                             ImplAttr());
            else
                rewriter.replaceOpWithNewOp<QueuePushDeferredOp>(op, queue, batch,
                                                                 adaptor.getOperands()[k + 2], fallback,
                                                                 sourceType, argAttrs, TopologyAttr(),
                                                                 IntegerAttr(), ImplAttr());
            return success();
        }

        if (kind == SemanticOpKind::QueuePush) {
            if (op.getNumOperands() != k + 2)
                return op.emitError("wtc: push takes the queue and one value; this call has ")
                       << op.getNumOperands() << " operand(s)";
            rewriter.replaceOpWithNewOp<QueuePushOp>(op, queue, adaptor.getOperands()[k + 1],
                                                     fallback, sourceType, argAttrs, TopologyAttr(), IntegerAttr(), ImplAttr());
            return success();
        }

        // try_pop(T&): the queue, then the program's object the element is
        // moved into; the result, if any, says whether one was.
        if (kind == SemanticOpKind::QueueTryPopInto) {
            if (k != 0 || op.getNumOperands() != 2 || op.getNumResults() > 1)
                return op.emitError("wtc: try_pop into an object takes the queue and the object");
            rewriter.replaceOpWithNewOp<QueuePopOp>(op, op.getResultTypes(),
                                                    queue, ValueRange{adaptor.getOperands()[1]},
                                                    fallback, sourceType, argAttrs, TopologyAttr(), IntegerAttr(), ImplAttr(),
                                                    UnitAttr(), rewriter.getUnitAttr(), UnitAttr());
            return success();
        }

        // pop_wait(T&) and pop_wait_for(T&, nanoseconds): a transfer that
        // waits, with the limit after the object; the result, if any, says
        // whether an element came.
        if (waitOp) {
            const unsigned want = kind == SemanticOpKind::QueuePopWaitFor ? 3 : 2;
            if (k != 0 || op.getNumOperands() != want || op.getNumResults() > 1)
                return op.emitError(want == 3 ? "wtc: pop_wait_for takes the queue, the object and the limit"
                                              : "wtc: pop_wait takes the queue and the object");
            rewriter.replaceOpWithNewOp<QueuePopOp>(op, op.getResultTypes(), queue,
                                                    adaptor.getOperands().drop_front(1), fallback, sourceType,
                                                    argAttrs, TopologyAttr(), IntegerAttr(), ImplAttr(),
                                                    UnitAttr(), rewriter.getUnitAttr(), rewriter.getUnitAttr());
            return success();
        }

        // try_pop(). Two shapes: a real result (hand-written IR), or a return
        // slot in operand 0 and no result (what the compiler emits).
        if (k == 1) {
            if (op.getNumOperands() != 2 || op.getNumResults() != 0)
                return op.emitError("wtc: try_pop with a return slot must have two operands and no result");
            rewriter.replaceOpWithNewOp<QueuePopOp>(op, /*resultTypes=*/TypeRange{},
                                                    queue, ValueRange{adaptor.getOperands()[0]},
                                                    fallback, sourceType, argAttrs, TopologyAttr(), IntegerAttr(), ImplAttr(),
                                                    UnitAttr(), UnitAttr(), UnitAttr());
            return success();
        }
        if (op.getNumOperands() != 1 || op.getNumResults() != 1)
            return op.emitError("wtc: try_pop without a return slot must have one operand and one result");
        rewriter.replaceOpWithNewOp<QueuePopOp>(op, TypeRange{op.getResult().getType()},
                                                queue, /*args=*/ValueRange{},
                                                fallback, sourceType, argAttrs, TopologyAttr(), IntegerAttr(), ImplAttr(),
                                                UnitAttr(), UnitAttr(), UnitAttr());
        return success();
    }
};

struct LiftToWTCPass : public PassWrapper<LiftToWTCPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LiftToWTCPass)

    StringRef getArgument() const final { return "lift-cir-to-wtc"; }
    StringRef getDescription() const final {
        return "Lift calls into the wtc shim library up to wtc dialect operations";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        MLIRContext *context = &getContext();
        CirToWTCConversionTarget target(*context);
        RewritePatternSet patterns(context);
        patterns.add<LiftMutexCallPattern, LiftQueueCallPattern>(context);
        if (failed(applyPartialConversion(getOperation(), target, std::move(patterns))))
            signalPassFailure();
    }
};

} // namespace

namespace mlir::wtc {
    void registerLiftCIRToWTCPass() {
        PassRegistration<LiftToWTCPass>();
    }
}
