#include "WTC/Transforms/Passes.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"

#include <type_traits>

// Lowers wtc operations back to the CIR calls they were lifted from. The
// call is rebuilt from what lifting recorded: the callee, the handle's CIR
// type, and the original argument attributes, which restore the hidden
// return-slot marker where there was one. An operation that carries none of
// that cannot be lowered and says so; it used to segfault.
//
// The casts this leaves behind are reconciled by --reconcile-unrealized-casts,
// which must follow this pass before cir-translate sees the module.

namespace {

struct WTCToCirConversionTarget : public ConversionTarget {
    WTCToCirConversionTarget(MLIRContext &ctx) : ConversionTarget(ctx) {
        addLegalDialect<cir::CIRDialect>();
        addLegalOp<mlir::UnrealizedConversionCastOp>();
        addIllegalDialect<WTCDialect>();
    }
};

template <class Op>
LogicalResult requireFallback(Op op, FlatSymbolRefAttr callee, TypeAttr sourceType) {
    if (!callee || !sourceType)
        return op.emitError("wtc: cannot lower an operation that carries no fallback implementation; "
                            "it was not produced by lifting");
    return success();
}

cir::CallOp rebuildCall(ConversionPatternRewriter &rewriter, Location loc,
                        FlatSymbolRefAttr callee, Type resultType, ValueRange operands,
                        ArrayAttr argAttrs) {
    auto call = cir::CallOp::create(rewriter, loc, callee, resultType, operands);
    if (argAttrs)
        call.setArgAttrsAttr(argAttrs);
    return call;
}

struct LowerMutexLockPattern : public OpConversionPattern<LockOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(LockOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        if (failed(requireFallback(op, op.getFallbackImplAttr(), op.getSourceMutexTypeAttr())))
            return failure();
        Value mutex = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), op.getSourceMutexTypeAttr().getValue(), adaptor.getMutex()).getResult(0);
        rebuildCall(rewriter, op.getLoc(), op.getFallbackImplAttr(), Type{}, ValueRange{mutex},
                    op.getSourceArgAttrsAttr());
        rewriter.eraseOp(op);
        return success();
    }
};

struct LowerMutexUnlockPattern : public OpConversionPattern<UnlockOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(UnlockOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        if (failed(requireFallback(op, op.getFallbackImplAttr(), op.getSourceMutexTypeAttr())))
            return failure();
        Value mutex = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), op.getSourceMutexTypeAttr().getValue(), adaptor.getMutex()).getResult(0);
        rebuildCall(rewriter, op.getLoc(), op.getFallbackImplAttr(), Type{}, ValueRange{mutex},
                    op.getSourceArgAttrsAttr());
        rewriter.eraseOp(op);
        return success();
    }
};

struct LowerQueuePushPattern : public OpConversionPattern<QueuePushOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(QueuePushOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        if (failed(requireFallback(op, op.getFallbackImplAttr(), op.getSourceQueueTypeAttr())))
            return failure();
        Value queue = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), op.getSourceQueueTypeAttr().getValue(), adaptor.getQueue()).getResult(0);
        rebuildCall(rewriter, op.getLoc(), op.getFallbackImplAttr(), Type{},
                    ValueRange{queue, adaptor.getValue()}, op.getSourceArgAttrsAttr());
        rewriter.eraseOp(op);
        return success();
    }
};

struct LowerQueuePopPattern : public OpConversionPattern<QueuePopOp> {
    using OpConversionPattern::OpConversionPattern;
    LogicalResult matchAndRewrite(QueuePopOp op, OpAdaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        if (failed(requireFallback(op, op.getFallbackImplAttr(), op.getSourceQueueTypeAttr())))
            return failure();
        Value queue = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), op.getSourceQueueTypeAttr().getValue(), adaptor.getQueue()).getResult(0);
        if (adaptor.getOut()) {
            // Return-slot form: the slot comes first, as in the original call.
            rebuildCall(rewriter, op.getLoc(), op.getFallbackImplAttr(), Type{},
                        ValueRange{adaptor.getOut(), queue}, op.getSourceArgAttrsAttr());
            rewriter.eraseOp(op);
            return success();
        }
        auto call = rebuildCall(rewriter, op.getLoc(), op.getFallbackImplAttr(),
                                op.getValue().getType(), ValueRange{queue}, op.getSourceArgAttrsAttr());
        rewriter.replaceOp(op, call->getResults());
        return success();
    }
};

// The three batch operations lower alike: the queue cast back to its source
// type, then the batch and, for push_deferred, the value.
template <class Op>
struct LowerQueueBatchPattern : public OpConversionPattern<Op> {
    using OpConversionPattern<Op>::OpConversionPattern;
    LogicalResult matchAndRewrite(Op op, typename Op::Adaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        if (failed(requireFallback(op, op.getFallbackImplAttr(), op.getSourceQueueTypeAttr())))
            return failure();
        Value queue = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), op.getSourceQueueTypeAttr().getValue(), adaptor.getQueue()).getResult(0);
        llvm::SmallVector<Value, 3> operands{queue, adaptor.getBatch()};
        if constexpr (std::is_same_v<Op, QueuePushDeferredOp>) operands.push_back(adaptor.getValue());
        rebuildCall(rewriter, op.getLoc(), op.getFallbackImplAttr(), Type{}, operands,
                    op.getSourceArgAttrsAttr());
        rewriter.eraseOp(op);
        return success();
    }
};

// The three lock-batch operations lower alike: the mutex cast back to its
// source type, then the batch.
template <class Op>
struct LowerLockBatchPattern : public OpConversionPattern<Op> {
    using OpConversionPattern<Op>::OpConversionPattern;
    LogicalResult matchAndRewrite(Op op, typename Op::Adaptor adaptor,
                                  ConversionPatternRewriter &rewriter) const override {
        if (failed(requireFallback(op, op.getFallbackImplAttr(), op.getSourceMutexTypeAttr())))
            return failure();
        Value mutex = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), op.getSourceMutexTypeAttr().getValue(), adaptor.getMutex()).getResult(0);
        rebuildCall(rewriter, op.getLoc(), op.getFallbackImplAttr(), Type{},
                    ValueRange{mutex, adaptor.getBatch()}, op.getSourceArgAttrsAttr());
        rewriter.eraseOp(op);
        return success();
    }
};

struct LowerWTCToCIRPass : public PassWrapper<LowerWTCToCIRPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerWTCToCIRPass)

    StringRef getArgument() const final { return "lower-wtc-to-cir"; }
    StringRef getDescription() const final {
        return "Lower wtc dialect operations back down to CIR calls "
               "(follow with --reconcile-unrealized-casts)";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        MLIRContext *context = &getContext();
        WTCToCirConversionTarget target(*context);
        RewritePatternSet patterns(context);
        patterns.add<LowerMutexLockPattern, LowerMutexUnlockPattern,
                     LowerQueuePushPattern, LowerQueuePopPattern,
                     LowerQueueBatchPattern<QueueBatchBeginOp>,
                     LowerQueueBatchPattern<QueuePushDeferredOp>,
                     LowerQueueBatchPattern<QueueBatchEndOp>,
                     LowerLockBatchPattern<LockBatchBeginOp>,
                     LowerLockBatchPattern<LockBatchStepOp>,
                     LowerLockBatchPattern<LockBatchEndOp>>(context);
        if (failed(applyPartialConversion(getOperation(), target, std::move(patterns))))
            return signalPassFailure();

        // A module is not pure CIR while it still carries attributes of our
        // dialect. The verdict is left on globals and allocas for the
        // specialisation pass and for tests to read; by the time we are here
        // it has been consumed, and anything downstream that does not know
        // the dialect refuses the module. cir-translate does exactly that:
        // "attribute created with unregistered dialect", after a successful
        // compile, with the pipeline's last step the one that fails.
        StringRef ourPrefix = WTCDialect::getDialectNamespace();
        getOperation()->walk([&](Operation *op) {
            SmallVector<StringAttr, 4> drop;
            for (NamedAttribute attr : op->getDiscardableAttrs()) {
                StringRef name = attr.getName().getValue();
                if (name.starts_with(ourPrefix) &&
                    name.drop_front(ourPrefix.size()).starts_with("."))
                    drop.push_back(attr.getName());
            }
            for (StringAttr name : drop)
                op->removeDiscardableAttr(name);
        });
    }
};

} // namespace

namespace mlir::wtc {
    void registerLowerWTCToCIRPass() {
        PassRegistration<LowerWTCToCIRPass>();
    }
}
