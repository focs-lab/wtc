#include "WTC/CosynthOps.h"

using namespace mlir;
using namespace mlir::cosynth;

#include "mlir/Pass/Pass.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/Transforms/DialectConversion.h"

struct CosynthToCirConversionTarget : public ConversionTarget {
    CosynthToCirConversionTarget(MLIRContext &ctx) : ConversionTarget(ctx) {
        addLegalDialect<cir::CIRDialect>();
        addIllegalDialect<CosynthDialect>();
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

        Value mutexAsCir = rewriter.create<mlir::UnrealizedConversionCastOp>(
            op.getLoc(),
            sourceMutexType,
            adaptor.getMutex()
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

        Value mutexAsCir = rewriter.create<mlir::UnrealizedConversionCastOp>(
            op.getLoc(),
            sourceMutexType,
            adaptor.getMutex()
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

        Value queueAsCir = rewriter.create<mlir::UnrealizedConversionCastOp>(
            op.getLoc(),
            sourceQueueType,
            adaptor.getQueue()
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

        Value queueAsCir = rewriter.create<mlir::UnrealizedConversionCastOp>(
            op.getLoc(),
            sourceQueueType,
            adaptor.getQueue()
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

struct LowerCosynthToCIRPass
    : public PassWrapper<LowerCosynthToCIRPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LowerCosynthToCIRPass)
    
    StringRef getArgument() const final {
        return "lower-cosynth-to-cir";
    }
    StringRef getDescription() const final {
        return "Lower Cosynth calls to the CIR";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::cosynth::CosynthDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        MLIRContext *context = &getContext();
        CosynthToCirConversionTarget conversionTarget(*context);
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

namespace mlir::cosynth {
    void registerLowerCosynthToCIRPass() {
        PassRegistration<LowerCosynthToCIRPass>();
    }
}
