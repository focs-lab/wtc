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
            case SemanticOpKind::QueuePush:
            case SemanticOpKind::QueueTryPop:
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
        if (kind != SemanticOpKind::MutexLock && kind != SemanticOpKind::MutexUnlock)
            return failure();

        const unsigned k = firstRealOperand(op);
        if (op.getNumOperands() != k + 1)
            return op.emitError("wtc: a mutex operation takes exactly the mutex; this call has ")
                   << op.getNumOperands() << " operand(s)";

        Value mutex = castTo(rewriter, op.getLoc(), MutexType::get(rewriter.getContext()),
                             adaptor.getOperands()[k]);
        auto sourceType = TypeAttr::get(op.getOperand(k).getType());
        auto argAttrs = op.getArgAttrsAttr();

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
        if (kind != SemanticOpKind::QueuePush && kind != SemanticOpKind::QueueTryPop && !batchOp)
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

        // try_pop. Two shapes: a real result (hand-written IR), or a return
        // slot in operand 0 and no result (what the compiler emits).
        if (k == 1) {
            if (op.getNumOperands() != 2 || op.getNumResults() != 0)
                return op.emitError("wtc: try_pop with a return slot must have two operands and no result");
            rewriter.replaceOpWithNewOp<QueuePopOp>(op, /*resultTypes=*/TypeRange{},
                                                    queue, adaptor.getOperands()[0],
                                                    fallback, sourceType, argAttrs, TopologyAttr(), IntegerAttr(), ImplAttr(),
                                                    UnitAttr());
            return success();
        }
        if (op.getNumOperands() != 1 || op.getNumResults() != 1)
            return op.emitError("wtc: try_pop without a return slot must have one operand and one result");
        rewriter.replaceOpWithNewOp<QueuePopOp>(op, TypeRange{op.getResult().getType()},
                                                queue, /*out=*/Value(),
                                                fallback, sourceType, argAttrs, TopologyAttr(), IntegerAttr(), ImplAttr(),
                                                UnitAttr());
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
