#include "WTC/WTCOps.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"

#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Builders.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

using namespace mlir;
using namespace mlir::wtc;

namespace {

struct AltImpl {
    cir::RecordType recordType;
    Type elementType;
    FlatSymbolRefAttr constructSym;
    FlatSymbolRefAttr pushSym;
    FlatSymbolRefAttr popSym;
    FlatSymbolRefAttr dtorSym;
};  

llvm::SmallVector<cir::FuncOp> findDestructorsOf(ModuleOp module, cir::RecordType recordType) {
    llvm::SmallVector<cir::FuncOp> found;
    module.walk([&](cir::FuncOp funcOp) {
        auto dtorAttr = mlir::dyn_cast_or_null<cir::CXXDtorAttr>(
            funcOp.getCxxSpecialMemberAttr());
        if (dtorAttr && dtorAttr.getType() == recordType)
            found.push_back(funcOp);
    });
    return found;
}

}

struct SelectQueueImplementationPass
    : public PassWrapper<SelectQueueImplementationPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SelectQueueImplementationPass)

    StringRef getArgument() const final {
        return "select-queue-impl";
    }
    StringRef getDescription() const final {
        return "Substitute a proven-SPSC global queue's implementation with an anchor-instantiated alternative (e.g. wtc::spsc_queue<T,N>)";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        ModuleOp module = getOperation();

        llvm::SmallVector<AltImpl> altImpls = findAltImpls(module);
        if (altImpls.empty()) {
            llvm::outs() << "select-queue-impl: no alternative queue implementation "
                            "instantiated in this module (no anchor found); nothing to do\n";
            return;
        }

        llvm::DenseMap<cir::GlobalOp, llvm::SmallVector<Operation *>> globalOps;
        auto collect = [&](Operation *op, Value queueValue) {
            auto kindAttr = op->getAttrOfType<StringAttr>(kQueueKindAttrName);
            if (!kindAttr || kindAttr.getValue() != "SPSC")
                return;

            Value resolved = traceToLocalRoot(queueValue);
            if (mlir::isa<BlockArgument>(resolved))
                return; // parameter-passed -- out of scope for this pass

            auto getGlobalOp = resolved.getDefiningOp<cir::GetGlobalOp>();
            if (!getGlobalOp)
                return;
            auto nameAttr = getGlobalOp->getAttrOfType<FlatSymbolRefAttr>("name");
            if (!nameAttr)
                return;
            auto globalOp = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(
                getGlobalOp.getOperation(), nameAttr);
            if (!globalOp)
                return;

            globalOps[globalOp].push_back(op);
        };

        module.walk([&](QueuePushOp pushOp) { collect(pushOp.getOperation(), pushOp.getQueue()); });
        module.walk([&](QueuePopOp popOp) { collect(popOp.getOperation(), popOp.getQueue()); });

        for (auto &entry : globalOps)
            selectForGlobal(entry.first, entry.second, altImpls);
    }


    llvm::SmallVector<AltImpl> findAltImpls(ModuleOp module) {
        llvm::SmallVector<AltImpl> impls;

        auto recordOf = [](cir::FuncOp funcOp) -> cir::RecordType {
            if (funcOp.getNumArguments() < 1)
                return {};
            auto thisPtrType = mlir::dyn_cast<cir::PointerType>(funcOp.getArgument(0).getType());
            if (!thisPtrType)
                return {};
            return mlir::dyn_cast<cir::RecordType>(thisPtrType.getPointee());
        };

        module.walk([&](cir::FuncOp funcOp) {
            if (!funcHasAnnotation(funcOp, "wtc_queue_construction"))
                return;
            cir::RecordType recordType = recordOf(funcOp);
            if (!recordType)
                return;

            if (!recordType.getName().getValue().trim("\"").starts_with("wtc::spsc_queue<"))
                return;

            for (auto &impl : impls)
                if (impl.recordType == recordType)
                    return;

            AltImpl impl;
            impl.recordType = recordType;
            impl.constructSym = FlatSymbolRefAttr::get(funcOp.getSymNameAttr());

            auto dtorFuncs = findDestructorsOf(module, recordType);
            if (!dtorFuncs.empty())
                impl.dtorSym = FlatSymbolRefAttr::get(dtorFuncs.front().getSymNameAttr());
            impls.push_back(impl);
        });

        module.walk([&](cir::FuncOp funcOp) {
            bool isPush = funcHasAnnotation(funcOp, "wtc_queue_push");
            bool isPop = funcHasAnnotation(funcOp, "wtc_queue_try_pop");
            if (!isPush && !isPop)
                return;
            cir::RecordType recordType = recordOf(funcOp);
            if (!recordType)
                return;

            for (auto &impl : impls) {
                if (impl.recordType != recordType)
                    continue;
                if (isPush) {
                    impl.pushSym = FlatSymbolRefAttr::get(funcOp.getSymNameAttr());
                    impl.elementType = deriveQueueElementTypeFromPush(funcOp);
                } else {
                    impl.popSym = FlatSymbolRefAttr::get(funcOp.getSymNameAttr());
                }
            }
        });

        llvm::erase_if(impls, [](const AltImpl &impl) {
            return !impl.pushSym || !impl.popSym || !impl.elementType || !impl.dtorSym;
        });

        return impls;
    }

    void selectForGlobal(cir::GlobalOp globalOp, ArrayRef<Operation *> ops,
                          ArrayRef<AltImpl> altImpls) {
        Type elementType;
        if (auto pushOp = mlir::dyn_cast<QueuePushOp>(ops.front()))
            elementType = mlir::cast<QueueType>(pushOp.getQueue().getType()).getElementType();
        else
            elementType = mlir::cast<QueueType>(
                mlir::cast<QueuePopOp>(ops.front()).getQueue().getType()).getElementType();

        const AltImpl *chosen = nullptr;
        for (auto &impl : altImpls) {
            if (impl.elementType == elementType) {
                chosen = &impl;
                break;
            }
        }
        if (!chosen) {
            llvm::outs() << "select-queue-impl: no alternative implementation matches "
                            "the element type of @" << globalOp.getSymName()
                         << "; leaving it on the original implementation\n";
            return;
        }

        cir::PointerType newPtrType = cir::PointerType::get(chosen->recordType);
        ModuleOp module = globalOp->getParentOfType<ModuleOp>();

      
        auto oldRecordType = mlir::dyn_cast<cir::RecordType>(globalOp.getSymType());

        
        globalOp.setSymTypeAttr(TypeAttr::get(chosen->recordType));
        globalOp.setInitialValueAttr(cir::ZeroAttr::get(chosen->recordType));


        uint64_t newAlign = 8;
        if (auto recordLayouts = module->getAttrOfType<DictionaryAttr>("cir.record_layouts")) {
            StringRef recordName = chosen->recordType.getName().getValue().trim("\"");
            if (auto layoutAttr = mlir::dyn_cast_or_null<cir::RecordLayoutAttr>(
                    recordLayouts.get(recordName)))
                newAlign = layoutAttr.getRecordAlign();
        }
        globalOp.setAlignmentAttr(
            IntegerAttr::get(IntegerType::get(globalOp.getContext(), 64), newAlign));

        if (auto uses = SymbolTable::getSymbolUses(globalOp.getOperation(), module.getOperation())) {
            for (const SymbolTable::SymbolUse &use : *uses) {
                auto getGlobalOp = mlir::dyn_cast<cir::GetGlobalOp>(use.getUser());
                if (!getGlobalOp)
                    continue;

                getGlobalOp.getResult().setType(newPtrType);

                for (Operation *user : llvm::make_early_inc_range(getGlobalOp.getResult().getUsers())) {
                    auto callOp = mlir::dyn_cast<cir::CallOp>(user);
                    if (!callOp || !isWTCAnnotatedCall(callOp, "wtc_queue_construction"))
                        continue;

                    OpBuilder builder(callOp);
                    cir::CallOp::create(builder, callOp.getLoc(), chosen->constructSym,
                                         /*returnType=*/Type{}, ValueRange{callOp.getOperand(0)});
                    callOp.erase();
                }
            }
        }


        if (oldRecordType) {
            if (auto newDtor = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(
                    globalOp.getOperation(), chosen->dtorSym)) {

                for (cir::FuncOp oldDtor : findDestructorsOf(module, oldRecordType)) {
                    auto dtorUses = SymbolTable::getSymbolUses(oldDtor.getOperation(), module.getOperation());
                    if (!dtorUses)
                        continue;
                    for (const SymbolTable::SymbolUse &use : *dtorUses) {
                        auto dtorGetGlobalOp = mlir::dyn_cast<cir::GetGlobalOp>(use.getUser());
                        if (!dtorGetGlobalOp)
                            continue;
                        dtorGetGlobalOp->setAttr("name", chosen->dtorSym);
                        dtorGetGlobalOp.getResult().setType(
                            cir::PointerType::get(newDtor.getFunctionType()));
                    }
                }
            }
        }

   
        for (Operation *op : ops) {
            if (auto pushOp = mlir::dyn_cast<QueuePushOp>(op)) {
                pushOp.setFallbackImplAttr(chosen->pushSym);
                pushOp.setSourceQueueTypeAttr(TypeAttr::get(newPtrType));
            } else if (auto popOp = mlir::dyn_cast<QueuePopOp>(op)) {
                popOp.setFallbackImplAttr(chosen->popSym);
                popOp.setSourceQueueTypeAttr(TypeAttr::get(newPtrType));
            }
        }

        llvm::outs() << "select-queue-impl: substituted "
                     << chosen->recordType.getName().getValue()
                     << " for @" << globalOp.getSymName() << "\n";
    }
};

namespace mlir::wtc {
    void registerSelectQueueImplementationPass() {
        PassRegistration<SelectQueueImplementationPass>();
    }
}
