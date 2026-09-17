#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "mlir/Pass/Pass.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/Transforms/DialectConversion.h"

struct WTCToCirConversionTarget : public ConversionTarget {
    WTCToCirConversionTarget(MLIRContext &ctx) : ConversionTarget(ctx) {
        addLegalDialect<cir::CIRDialect>();
        addIllegalDialect<WTCDialect>();
    }
};

struct LowerMutexLockPattern : public OpConversionPattern<LockOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(
        LockOp op, 
        OpAdaptor adaptor, 
        ConversionPatternRewriter &rewriter
    ) const override {
        auto calleeAttr = op.getFallbackImplAttr();
        auto sourceMutexType = op.getSourceMutexTypeAttr().getValue();

        Value mutexAsCir = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), sourceMutexType, adaptor.getMutex()
        ).getResult(0);

        rewriter.replaceOpWithNewOp<cir::CallOp>(
            op,
            calleeAttr,
            /*returnType=*/Type{},
            ValueRange{mutexAsCir}
        );

        return success();
    }
};

struct LowerMutexUnlockPattern : public OpConversionPattern<UnlockOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(
        UnlockOp op, 
        OpAdaptor adaptor, 
        ConversionPatternRewriter &rewriter
    ) const override {
        auto calleeAttr = op.getFallbackImplAttr();

        auto sourceMutexType = op.getSourceMutexTypeAttr().getValue();

        Value mutexAsCir = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), sourceMutexType, adaptor.getMutex()
        ).getResult(0);

        rewriter.replaceOpWithNewOp<cir::CallOp>(
            op,
            calleeAttr,
            /*returnType=*/Type{},
            ValueRange{mutexAsCir}
        );

        return success();
    }
};

struct LowerQueuePushPattern : public OpConversionPattern<QueuePushOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(
        QueuePushOp op,
        OpAdaptor adaptor,
        ConversionPatternRewriter &rewriter
    ) const override {
        auto calleeAttr = op.getFallbackImplAttr();
        auto sourceQueueType = op.getSourceQueueTypeAttr().getValue();

        Value queueAsCir = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), sourceQueueType, adaptor.getQueue()
        ).getResult(0);

        rewriter.replaceOpWithNewOp<cir::CallOp>(
            op,
            calleeAttr,
            /*returnType=*/Type{},
            ValueRange{queueAsCir, adaptor.getValue()}
        );

        return success();
    }
};

struct LowerQueuePopPattern : public OpConversionPattern<QueuePopOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(
        QueuePopOp op,
        OpAdaptor adaptor,
        ConversionPatternRewriter &rewriter
    ) const override {
        auto calleeAttr = op.getFallbackImplAttr();
        auto sourceQueueType = op.getSourceQueueTypeAttr().getValue();

        Value queueAsCir = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), sourceQueueType, adaptor.getQueue()
        ).getResult(0);

        rewriter.replaceOpWithNewOp<cir::CallOp>(
            op,
            calleeAttr,
            /*returnType=*/op.getResult().getType(),
            ValueRange{queueAsCir}
        );

        return success();
    }
};

struct LowerWTCToCIRPass
    : public PassWrapper<LowerWTCToCIRPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerWTCToCIRPass)
    
    StringRef getArgument() const final {
        return "lower-wtc-to-cir";
    }
    StringRef getDescription() const final {
        return "Lower wtc dialect operations back down to CIR calls";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        MLIRContext *context = &getContext();
        WTCToCirConversionTarget conversionTarget(*context);
        RewritePatternSet patterns(context);
        patterns.add<LowerMutexLockPattern>(context);
        patterns.add<LowerMutexUnlockPattern>(context);
        patterns.add<LowerQueuePushPattern>(context);
        patterns.add<LowerQueuePopPattern>(context);

        if (failed(applyPartialConversion(getOperation(), conversionTarget, std::move(patterns)))) {
            signalPassFailure();
        }
    }
};

namespace mlir::wtc {
    void registerLowerWTCToCIRPass() {
        PassRegistration<LowerWTCToCIRPass>();
    }
}
