#include "WTC/WTCOps.h"
#include "WTC/Transforms/Utils.h"

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
        if (!calleeAttr || !op.getSourceMutexTypeAttr())
            return rewriter.notifyMatchFailure(op, "no fallback_impl or source_mutex_type");
        auto sourceMutexType = op.getSourceMutexTypeAttr().getValue();

        Value mutexAsCir = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), sourceMutexType, adaptor.getMutex()
        ).getResult(0);

        auto call = rewriter.replaceOpWithNewOp<cir::CallOp>(
            op,
            calleeAttr,
            /*returnType=*/Type{},
            ValueRange{mutexAsCir}
        );
        copyArgAttrsFromCallee(call, SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(call, calleeAttr));

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
        if (!calleeAttr || !op.getSourceMutexTypeAttr())
            return rewriter.notifyMatchFailure(op, "no fallback_impl or source_mutex_type");
        auto sourceMutexType = op.getSourceMutexTypeAttr().getValue();

        Value mutexAsCir = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), sourceMutexType, adaptor.getMutex()
        ).getResult(0);

        auto call = rewriter.replaceOpWithNewOp<cir::CallOp>(
            op,
            calleeAttr,
            /*returnType=*/Type{},
            ValueRange{mutexAsCir}
        );
        copyArgAttrsFromCallee(call, SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(call, calleeAttr));

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
        if (!calleeAttr || !op.getSourceQueueTypeAttr())
            return rewriter.notifyMatchFailure(op, "no fallback_impl or source_queue_type");
        auto sourceQueueType = op.getSourceQueueTypeAttr().getValue();

        Value queueAsCir = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), sourceQueueType, adaptor.getQueue()
        ).getResult(0);

        // The push op has no result, but the function it lowers to may: a
        // substituted implementation can return a status. The call has to
        // match the callee's signature either way.
        auto calleeFunc = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(op, calleeAttr);
        Type returnType = calleeFunc ? calleeFunc.getFunctionType().getReturnType() : Type{};

        auto call = cir::CallOp::create(
            rewriter,
            op.getLoc(),
            calleeAttr,
            returnType,
            ValueRange{queueAsCir, adaptor.getValue()}
        );
        copyArgAttrsFromCallee(call, calleeFunc);
        rewriter.eraseOp(op);

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
        if (!calleeAttr || !op.getSourceQueueTypeAttr())
            return rewriter.notifyMatchFailure(op, "no fallback_impl or source_queue_type");
        auto sourceQueueType = op.getSourceQueueTypeAttr().getValue();

        Value queueAsCir = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), sourceQueueType, adaptor.getQueue()
        ).getResult(0);

        auto calleeFunc = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(op, calleeAttr);

        // A value returned indirectly goes back where the callee expects
        // its hidden argument, which the callee's signature marks.
        SmallVector<Value> args{queueAsCir};
        Type returnType;
        if (Value slot = adaptor.getResultSlot()) {
            unsigned slotIndex = calleeFunc ? sretArgIndex(calleeFunc).value_or(0) : 0;
            args.insert(args.begin() + std::min<size_t>(slotIndex, args.size()), slot);
        } else {
            returnType = op.getValue().getType();
        }

        auto call = cir::CallOp::create(rewriter, op.getLoc(), calleeAttr, returnType, args);
        copyArgAttrsFromCallee(call, calleeFunc);
        rewriter.replaceOp(op, call.getResults());

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

        // Lowering restores the original call, so it needs both the callee
        // and the original object type. Both are optional on the ops, and a
        // transform may emit ops without them; say which op and stop rather
        // than leave the patterns to fail to legalize it.
        bool missing = false;
        getOperation().walk([&](Operation *op) {
            Attribute impl, sourceType;
            if (auto lockOp = mlir::dyn_cast<LockOp>(op)) {
                impl = lockOp.getFallbackImplAttr();
                sourceType = lockOp.getSourceMutexTypeAttr();
            } else if (auto unlockOp = mlir::dyn_cast<UnlockOp>(op)) {
                impl = unlockOp.getFallbackImplAttr();
                sourceType = unlockOp.getSourceMutexTypeAttr();
            } else if (auto pushOp = mlir::dyn_cast<QueuePushOp>(op)) {
                impl = pushOp.getFallbackImplAttr();
                sourceType = pushOp.getSourceQueueTypeAttr();
            } else if (auto popOp = mlir::dyn_cast<QueuePopOp>(op)) {
                impl = popOp.getFallbackImplAttr();
                sourceType = popOp.getSourceQueueTypeAttr();
            } else {
                return;
            }
            if (impl && sourceType)
                return;
            op->emitOpError("cannot be lowered: it carries no ")
                << (impl ? "source type" : "fallback_impl")
                << " to reconstruct the original call from";
            missing = true;
        });
        if (missing)
            return signalPassFailure();

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
