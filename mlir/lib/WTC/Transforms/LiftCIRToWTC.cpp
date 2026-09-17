#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/CosynthOps.h"

using namespace mlir;
using namespace mlir::cosynth;

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/StringRef.h"

struct CirToCosynthConversionTarget : public ConversionTarget {
    CirToCosynthConversionTarget(MLIRContext &ctx) : ConversionTarget(ctx) {
        addLegalDialect<CosynthDialect>();
        addLegalDialect<cir::CIRDialect>();
        addLegalOp<mlir::UnrealizedConversionCastOp>();

        addDynamicallyLegalOp<cir::CallOp>([](cir::CallOp op) {
            return !isCosynthAnnotatedCall(op, "cosynth_mutex_lock") 
                && !isCosynthAnnotatedCall(op, "cosynth_mutex_unlock")
                && !isCosynthAnnotatedCall(op, "cosynth_queue_push")
                && !isCosynthAnnotatedCall(op, "cosynth_queue_try_pop");
        });
    }
};

class CirToCosynthTypeConverter : public TypeConverter {
public:
    CirToCosynthTypeConverter(MLIRContext *ctx) {
        addConversion([](Type t) { return t; });
        addConversion([ctx](cir::PointerType ptrTy) -> std::optional<Type> {
            auto recTy = mlir::dyn_cast<cir::RecordType>(ptrTy.getPointee());
            
            if (!recTy) return std::nullopt;
            if (recTy.getName().getValue().trim("\"") == "concur::mutex") 
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

        bool isLock = isCosynthAnnotatedCall(op, "cosynth_mutex_lock");
        bool isUnlock = isCosynthAnnotatedCall(op, "cosynth_mutex_unlock");

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
        bool isPush = isCosynthAnnotatedCall(op, "cosynth_queue_push");
        bool isPop = isCosynthAnnotatedCall(op, "cosynth_queue_try_pop");

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

struct LiftToCosynthPass 
    : public PassWrapper<LiftToCosynthPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LiftToCosynthPass)
    
    StringRef getArgument() const final {
        return "lift-cir-to-cosynth";
    }
    StringRef getDescription() const final {
        return "Lift CIR shim calls to the CoSynth dialect";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::cosynth::CosynthDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        MLIRContext *context = &getContext();

        CirToCosynthConversionTarget conversionTarget(*context);
        CirToCosynthTypeConverter typeConverter(context);
        RewritePatternSet patterns(context);

        patterns.add<LiftMutexCallPattern>(typeConverter, context);
        patterns.add<LiftQueueCallPattern>(typeConverter, context);

        if (failed(applyPartialConversion(getOperation(), conversionTarget, std::move(patterns)))) {
            signalPassFailure();
        }
    }
};

namespace mlir::cosynth {
    void registerLiftCIRToCosynthPass() {
        PassRegistration<LiftToCosynthPass>();
    }
}