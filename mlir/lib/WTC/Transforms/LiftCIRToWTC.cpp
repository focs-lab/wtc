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
        bool isLock = isWTCAnnotatedCall(op, "wtc_mutex_lock");
        bool isUnlock = isWTCAnnotatedCall(op, "wtc_mutex_unlock");

        if (!isLock && !isUnlock)
            return failure();

        if (op.getArgOperands().size() != 1) {
            op.emitError("expected a mutex call to take exactly the mutex, got ")
                << op.getArgOperands().size() << " arguments";
            return failure();
        }
        Value mutex = adaptor.getOperands()[0];

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

        // Where the target returns try_pop's shared_ptr indirectly, the call
        // has no result and a hidden argument the callee marks sret; the
        // queue is the argument after it. Reading operands by fixed index
        // without accounting for that is what used to crash.
        auto callee = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(op, op.getCalleeAttr());
        std::optional<unsigned> sret = callee ? sretArgIndex(callee) : std::nullopt;
        if (sret && *sret != 0) {
            op.emitError("expected the hidden result argument of a queue call to come first, got argument ")
                << *sret;
            return failure();
        }
        unsigned self = sret ? 1 : 0;

        auto args = op.getArgOperands();
        unsigned expected = self + (isPush ? 2 : 1);
        if (args.size() != expected) {
            op.emitError("expected a queue ")
                << (isPush ? "push" : "try_pop") << " call to take " << expected
                << " arguments, got " << args.size();
            return failure();
        }
        if (!isPush && !sret && op.getNumResults() != 1) {
            op.emitError("expected a queue try_pop call to return its value directly "
                         "or through an sret argument");
            return failure();
        }

        Type elementType;
        if (isPush) {
            if (auto valuePtrType = mlir::dyn_cast<cir::PointerType>(args[self + 1].getType()))
                elementType = valuePtrType.getPointee();
        } else {
            // try_pop returns std::shared_ptr<T>, whose first member is T*.
            Type returned;
            if (!sret)
                returned = op.getResult().getType();
            else if (auto sretType = mlir::dyn_cast<TypeAttr>(callee.getArgAttr(*sret, "llvm.sret")))
                returned = sretType.getValue();
            if (auto recordType = mlir::dyn_cast_if_present<cir::RecordType>(returned)) {
                auto members = recordType.getMembers();
                if (!members.empty())
                    if (auto objectPtrType = mlir::dyn_cast<cir::PointerType>(members[0]))
                        elementType = objectPtrType.getPointee();
            }
        }
        if (!elementType) {
            op.emitError("cannot recover the queue's element type from this call's signature");
            return failure();
        }

        auto queueType = QueueType::get(rewriter.getContext(), elementType);
        Value queue = mlir::UnrealizedConversionCastOp::create(
            rewriter, op.getLoc(), queueType, adaptor.getOperands()[self]
        ).getResult(0);

        auto fallbackImplAttr = FlatSymbolRefAttr::get(
            rewriter.getContext(),
            op.getCalleeAttr().getRootReference().getValue()
        );

        auto sourceQueueTypeAttr = TypeAttr::get(args[self].getType());

        if (isPush) {
            rewriter.replaceOpWithNewOp<QueuePushOp>(op, queue, adaptor.getOperands()[self + 1], IntegerAttr(), fallbackImplAttr, sourceQueueTypeAttr);
        } else if (sret) {
            rewriter.replaceOpWithNewOp<QueuePopOp>(op, /*value=*/Type{}, queue, adaptor.getOperands()[0], IntegerAttr(), fallbackImplAttr, sourceQueueTypeAttr);
        } else {
            rewriter.replaceOpWithNewOp<QueuePopOp>(op, op.getResult().getType(), queue, /*result_slot=*/Value{}, IntegerAttr(), fallbackImplAttr, sourceQueueTypeAttr);
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