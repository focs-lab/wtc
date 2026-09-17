#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/StringRef.h"

struct CirToWTCConversionTarget : public ConversionTarget {
    CirToWTCConversionTarget(MLIRContext &ctx) : ConversionTarget(ctx) {
        addLegalDialect<WTCDialect>();
        addLegalDialect<cir::CIRDialect>();
        addLegalOp<mlir::UnrealizedConversionCastOp>();

        addDynamicallyLegalOp<cir::CallOp>([](cir::CallOp op) {
            return !isWTCAnnotatedCall(op, "wtc_mutex_lock") 
                && !isWTCAnnotatedCall(op, "wtc_mutex_unlock")
                && !isWTCAnnotatedCall(op, "wtc_queue_push")
                && !isWTCAnnotatedCall(op, "wtc_queue_try_pop");
        });
    }
};

class CirToWTCTypeConverter : public TypeConverter {
public:
    CirToWTCTypeConverter(MLIRContext *ctx) {
        addConversion([](Type t) { return t; });
        addConversion([ctx](cir::PointerType ptrTy) -> std::optional<Type> {
            auto recTy = mlir::dyn_cast<cir::RecordType>(ptrTy.getPointee());
            
            if (!recTy) return std::nullopt;
            if (recTy.getName().getValue().trim("\"") == "wtc::mutex") 
                return MutexType::get(ctx);
            
            return std::nullopt;
        });

        addTargetMaterialization([](OpBuilder &builder,
                                 MutexType resultType,
                                 ValueRange inputs,
                                 Location loc) -> Value {
            return mlir::UnrealizedConversionCastOp::create(builder, loc, resultType, inputs)
                .getResult(0);
        });
        addSourceMaterialization([](OpBuilder &builder,
                                    cir::PointerType resultType,
                                    ValueRange inputs,
                                    Location loc) -> Value {
            return mlir::UnrealizedConversionCastOp::create(builder, loc, resultType, inputs)
                .getResult(0);
        });
    }
};

struct LiftMutexCallPattern : public OpConversionPattern<cir::CallOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(
        cir::CallOp op, 
        OpAdaptor adaptor, 
        ConversionPatternRewriter &rewriter
    ) const override {
        Value mutex = adaptor.getOperands()[0];

        bool isLock = isWTCAnnotatedCall(op, "wtc_mutex_lock");
        bool isUnlock = isWTCAnnotatedCall(op, "wtc_mutex_unlock");

        if (!isLock && !isUnlock)
            return failure();

        auto fallbackImplAttr = FlatSymbolRefAttr::get(
            rewriter.getContext(),
            op.getCalleeAttr().getRootReference().getValue()
        );

        Type originalMutexType = op.getOperand(0).getType();
        auto sourceMutexTypeAttr = TypeAttr::get(originalMutexType);

        if (isLock) {
            rewriter.replaceOpWithNewOp<LockOp>(op, mutex, fallbackImplAttr, sourceMutexTypeAttr);
        } else {
            rewriter.replaceOpWithNewOp<UnlockOp>(op, mutex, fallbackImplAttr, sourceMutexTypeAttr);
        }

        return success();
    }
};

struct LiftQueueCallPattern : public OpConversionPattern<cir::CallOp> {
    using OpConversionPattern::OpConversionPattern;

    LogicalResult matchAndRewrite(
        cir::CallOp op,
        OpAdaptor adaptor,
        ConversionPatternRewriter &rewriter
    ) const override {
        bool isPush = isWTCAnnotatedCall(op, "wtc_queue_push");
        bool isPop = isWTCAnnotatedCall(op, "wtc_queue_try_pop");

        if (!isPush && !isPop)
            return failure();

        Type elementType;
        if (isPush) {
            auto valuePtrType = mlir::cast<cir::PointerType>(op.getOperand(1).getType());
            elementType = valuePtrType.getPointee();
        } else {
            auto resultRecordType = mlir::cast<cir::RecordType>(op.getResult().getType());
            auto objectPtrType = mlir::cast<cir::PointerType>(resultRecordType.getMembers()[0]);
            elementType = objectPtrType.getPointee();
        }

        auto queueType = QueueType::get(rewriter.getContext(), elementType);
        Value queue = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), queueType, adaptor.getOperands()[0]
        ).getResult(0);

        auto fallbackImplAttr = FlatSymbolRefAttr::get(
            rewriter.getContext(),
            op.getCalleeAttr().getRootReference().getValue()
        );

        auto sourceQueueTypeAttr = TypeAttr::get(op.getOperand(0).getType());

        if (isPush) {
            rewriter.replaceOpWithNewOp<QueuePushOp>(op, queue, adaptor.getOperands()[1], IntegerAttr(), fallbackImplAttr, sourceQueueTypeAttr);
        } else {
            rewriter.replaceOpWithNewOp<QueuePopOp>(op, op.getResult().getType(), queue, IntegerAttr(), fallbackImplAttr, sourceQueueTypeAttr);
        }

        return success();
    }
};

struct LiftToWTCPass 
    : public PassWrapper<LiftToWTCPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LiftToWTCPass)
    
    StringRef getArgument() const final {
        return "lift-cir-to-wtc";
    }
    StringRef getDescription() const final {
        return "Lift calls into the wtc shim library up to wtc dialect operations";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        MLIRContext *context = &getContext();

        CirToWTCConversionTarget conversionTarget(*context);
        CirToWTCTypeConverter typeConverter(context);
        RewritePatternSet patterns(context);

        patterns.add<LiftMutexCallPattern>(typeConverter, context);
        patterns.add<LiftQueueCallPattern>(typeConverter, context);

        if (failed(applyPartialConversion(getOperation(), conversionTarget, std::move(patterns)))) {
            signalPassFailure();
        }
    }
};

namespace mlir::wtc {
    void registerLiftCIRToWTCPass() {
        PassRegistration<LiftToWTCPass>();
    }
}