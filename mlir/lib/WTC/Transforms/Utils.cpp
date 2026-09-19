#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "mlir/IR/BuiltinOps.h"

namespace mlir::wtc {
    bool isWTCAnnotatedCall(cir::CallOp op, StringRef annotation) {
        auto calleeAttr = op.getCalleeAttr();
        if (!calleeAttr)
            return false;

        auto module = op->getParentOfType<mlir::ModuleOp>();

        auto *calleeFn = mlir::SymbolTable::lookupNearestSymbolFrom(
            module, calleeAttr);

        auto funcOp = mlir::dyn_cast<cir::FuncOp>(calleeFn);
        if (!funcOp)
            return false;

        auto annotations = funcOp.getAnnotationsAttr();
        if (!annotations) 
            return false;

        for (auto attr: annotations) {
            auto annotAttr = mlir::dyn_cast<cir::AnnotationAttr>(attr);
            if (!annotAttr) continue;
            if (annotAttr.getName().getValue() == annotation)
                return true;
        }
        return false;
    }

    bool funcHasAnnotation(cir::FuncOp funcOp, StringRef annotation) {
        if (!funcOp)
            return false;

        auto annotations = funcOp.getAnnotationsAttr();
        if (!annotations)
            return false;

        for (auto attr : annotations) {
            auto annotAttr = mlir::dyn_cast<cir::AnnotationAttr>(attr);
            if (!annotAttr) continue;
            if (annotAttr.getName().getValue() == annotation)
                return true;
        }
        return false;
    }

    Type deriveQueueElementTypeFromPush(cir::FuncOp pushFunc) {
        if (!pushFunc || pushFunc.getNumArguments() < 2)
            return {};
        auto valuePtrType = mlir::dyn_cast<cir::PointerType>(pushFunc.getArgument(1).getType());
        if (!valuePtrType)
            return {};
        return valuePtrType.getPointee();
    }

    Type deriveQueueElementTypeFromPop(cir::FuncOp popFunc) {
        if (!popFunc)
            return {};
        auto resultRecordType = mlir::dyn_cast<cir::RecordType>(popFunc.getFunctionType().getReturnType());
        if (!resultRecordType || resultRecordType.getMembers().empty())
            return {};
        auto objectPtrType = mlir::dyn_cast<cir::PointerType>(resultRecordType.getMembers()[0]);
        if (!objectPtrType)
            return {};
        return objectPtrType.getPointee();
    }

    SemanticOpKind getSemanticKind(cir::FuncOp funcOp) {
        if (!funcOp) return SemanticOpKind::Unknown;
        
        auto annotations = funcOp.getAnnotationsAttr();
        if (!annotations) return SemanticOpKind::Unknown;
        
        for (auto attr: annotations) {
            auto annotAttr = mlir::dyn_cast<cir::AnnotationAttr>(attr);
            if (!annotAttr) return SemanticOpKind::Unknown;

            StringRef name = annotAttr.getName().getValue();
            if (name == "wtc_thread_spawn") {
                return SemanticOpKind::ThreadSpawn;
            }
        }
        return SemanticOpKind::Unknown;
    }

    Value traceToLocalRoot(Value v) {
        while (true) {
            Operation* def = v.getDefiningOp();
            if (!def)
                return v;
            if (auto castOp = mlir::dyn_cast<cir::CastOp>(def)) {
                v = castOp.getSrc();
                continue;
            }
            if (auto uccOp = mlir::dyn_cast<mlir::UnrealizedConversionCastOp>(def)) {
                if (uccOp.getNumOperands() != 1)
                    return v;
                v = uccOp.getOperand(0);
                continue;
            }
            return v;
        }
    }
}