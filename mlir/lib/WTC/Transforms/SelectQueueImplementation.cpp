#include "WTC/WTCOps.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"

#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/SmallVector.h"

using namespace mlir;
using namespace mlir::wtc;

// Substitutes the storage of a global wtc::queue<T> proven SPSC with
// runtime/spsc_queue.hpp's implementation for the same T.
//
// The substitute is found in the module, not synthesised: the shim's queue
// constructor odr-uses an anchor (annotated wtc_anchor) that instantiates
// wtc::runtime::spsc_queue<T>, so every module using queue<T> also carries
// spsc_queue<T>'s constructor, push, try_pop and destructor, each marked
// wtc_impl_spsc_*. The anchor is erased afterwards.
//
// Only globals are substituted, and only when every use of the global is
// one this pass knows how to retarget: the queue's own push and pop, its
// construction, and its destructor's registration with __cxa_atexit. A
// global that is used any other way keeps its original type, since that
// use would be left calling the original implementation on the new
// storage.

namespace {

constexpr StringRef kSpscPrefix = "wtc_impl_spsc_";

struct AltImpl {
    cir::RecordType recordType;
    Type elementType;
    FlatSymbolRefAttr construct;
    FlatSymbolRefAttr push;
    FlatSymbolRefAttr tryPop;
    FlatSymbolRefAttr dtor;

    bool complete() const { return elementType && construct && push && tryPop && dtor; }
};

// What one global's uses amount to, if every one of them can be retargeted.
struct GlobalUses {
    llvm::SmallVector<cir::GetGlobalOp> getGlobals;
    llvm::SmallVector<Operation *> queueOps;
    llvm::SmallVector<cir::CallOp> constructions;
    // The cir.get_global of the destructor each registration passes.
    llvm::SmallVector<cir::GetGlobalOp> dtorRefs;
};

cir::RecordType recordOfThis(cir::FuncOp funcOp) {
    if (funcOp.getNumArguments() < 1)
        return {};
    // Where try_pop returns indirectly, `this` follows the result slot.
    unsigned self = sretArgIndex(funcOp) == 0u ? 1 : 0;
    if (funcOp.getNumArguments() <= self)
        return {};
    // From the signature, not the entry block: a declaration has none.
    auto thisPtrType = mlir::dyn_cast<cir::PointerType>(funcOp.getFunctionType().getInput(self));
    if (!thisPtrType)
        return {};
    return mlir::dyn_cast<cir::RecordType>(thisPtrType.getPointee());
}

Value stripCasts(Value v) {
    while (auto castOp = v.getDefiningOp<cir::CastOp>())
        v = castOp.getSrc();
    return v;
}

}

struct SelectQueueImplementationPass
    : public PassWrapper<SelectQueueImplementationPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(SelectQueueImplementationPass)

    StringRef getArgument() const final {
        return "select-queue-impl";
    }
    StringRef getDescription() const final {
        return "Substitute wtc::runtime::spsc_queue<T> for a global wtc::queue<T> proven SPSC";
    }

    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        ModuleOp module = getOperation();
        llvm::SmallVector<AltImpl> altImpls = findAltImpls(module);

        for (cir::GlobalOp globalOp : llvm::to_vector(module.getOps<cir::GlobalOp>())) {
            auto kind = globalOp->getAttrOfType<StringAttr>(kQueueKindAttrName);
            if (!kind)
                continue;
            if (kind.getValue() != "SPSC") {
                globalOp.emitRemark("not substituted: the queue is ") << kind.getValue();
                continue;
            }
            selectForGlobal(module, globalOp, altImpls);
        }

        eraseAnchors(module);
    }

    llvm::SmallVector<AltImpl> findAltImpls(ModuleOp module) {
        llvm::SmallVector<AltImpl> impls;
        auto implFor = [&](cir::RecordType recordType) -> AltImpl & {
            for (AltImpl &impl : impls)
                if (impl.recordType == recordType)
                    return impl;
            impls.push_back({});
            impls.back().recordType = recordType;
            return impls.back();
        };

        // The complete and base constructors (C1, C2), and destructors (D1,
        // D2), are interchangeable for a class without virtual bases, which
        // spsc_queue is; the first one found is used.
        module.walk([&](cir::FuncOp funcOp) {
            auto sym = FlatSymbolRefAttr::get(funcOp.getSymNameAttr());
            auto setOnce = [&](FlatSymbolRefAttr &slot) { if (!slot) slot = sym; };

            if (funcHasAnnotation(funcOp, (kSpscPrefix + "construction").str())) {
                if (auto recordType = recordOfThis(funcOp))
                    setOnce(implFor(recordType).construct);
            } else if (funcHasAnnotation(funcOp, (kSpscPrefix + "dtor").str())) {
                if (auto recordType = recordOfThis(funcOp))
                    setOnce(implFor(recordType).dtor);
            } else if (funcHasAnnotation(funcOp, (kSpscPrefix + "push").str())) {
                if (auto recordType = recordOfThis(funcOp)) {
                    AltImpl &impl = implFor(recordType);
                    setOnce(impl.push);
                    impl.elementType = deriveQueueElementTypeFromPush(funcOp);
                }
            } else if (funcHasAnnotation(funcOp, (kSpscPrefix + "try_pop").str())) {
                if (auto recordType = recordOfThis(funcOp))
                    setOnce(implFor(recordType).tryPop);
            }
        });

        llvm::erase_if(impls, [](const AltImpl &impl) { return !impl.complete(); });
        return impls;
    }

    // Every use of the global, sorted into the kinds this pass can
    // retarget; std::nullopt, with a remark, at the first it cannot.
    std::optional<GlobalUses> collectUses(ModuleOp module, cir::GlobalOp globalOp) {
        GlobalUses result;
        auto refuse = [&](Operation *user, StringRef why) -> std::optional<GlobalUses> {
            globalOp.emitRemark("not substituted: ") << why;
            user->emitRemark("this use of @") << globalOp.getSymName() << " is the one";
            return std::nullopt;
        };

        auto uses = SymbolTable::getSymbolUses(globalOp.getOperation(), module.getOperation());
        if (!uses)
            return result;

        for (const SymbolTable::SymbolUse &use : *uses) {
            auto getGlobalOp = mlir::dyn_cast<cir::GetGlobalOp>(use.getUser());
            if (!getGlobalOp)
                return refuse(use.getUser(), "its address is used other than through cir.get_global");
            result.getGlobals.push_back(getGlobalOp);

            for (Operation *user : getGlobalOp.getResult().getUsers()) {
                if (auto castOp = mlir::dyn_cast<mlir::UnrealizedConversionCastOp>(user)) {
                    if (castOp.getNumResults() != 1 || !mlir::isa<QueueType>(castOp.getResult(0).getType()))
                        return refuse(user, "it is cast to something other than a wtc queue");
                    for (Operation *queueUser : castOp.getResult(0).getUsers()) {
                        if (!mlir::isa<QueuePushOp, QueuePopOp>(queueUser))
                            return refuse(queueUser, "the lifted queue is used by something other than push and pop");
                        auto opKind = queueUser->getAttrOfType<StringAttr>(kQueueKindAttrName);
                        if (!opKind || opKind.getValue() != "SPSC")
                            return refuse(queueUser, "an op on it can also reach a queue that is not SPSC");
                        result.queueOps.push_back(queueUser);
                    }
                    continue;
                }

                auto callOp = mlir::dyn_cast<cir::CallOp>(user);
                if (callOp && isWTCAnnotatedCall(callOp, "wtc_queue_construction") &&
                    !callOp.getArgOperands().empty() && callOp.getArgOperands()[0] == getGlobalOp.getResult()) {
                    result.constructions.push_back(callOp);
                    continue;
                }

                // __cxa_atexit(dtor, &global, dso_handle), both pointers cast
                // to their void forms first.
                auto castOp = mlir::dyn_cast<cir::CastOp>(user);
                if (castOp && castOp.getResult().hasOneUse()) {
                    auto atexit = mlir::dyn_cast<cir::CallOp>(*castOp.getResult().getUsers().begin());
                    auto callee = atexit ? atexit.getCalleeAttr() : FlatSymbolRefAttr();
                    if (callee && callee.getValue() == "__cxa_atexit" &&
                        atexit.getArgOperands().size() == 3 &&
                        atexit.getArgOperands()[1] == castOp.getResult()) {
                        auto dtorRef = stripCasts(atexit.getArgOperands()[0]).getDefiningOp<cir::GetGlobalOp>();
                        if (!dtorRef)
                            return refuse(atexit, "its destructor registration names no function");
                        // The reference is changed in place, so it must
                        // serve this registration and nothing else.
                        Value v = dtorRef.getResult();
                        while (v.hasOneUse() && mlir::isa<cir::CastOp>(*v.getUsers().begin()))
                            v = v.getUsers().begin()->getResult(0);
                        if (!dtorRef.getResult().hasOneUse() || !v.hasOneUse())
                            return refuse(atexit, "its destructor reference is shared with another use");
                        result.dtorRefs.push_back(dtorRef);
                        continue;
                    }
                }

                return refuse(user, "it has a use this pass cannot retarget");
            }
        }
        return result;
    }

    void selectForGlobal(ModuleOp module, cir::GlobalOp globalOp, ArrayRef<AltImpl> altImpls) {
        std::optional<GlobalUses> uses = collectUses(module, globalOp);
        if (!uses)
            return;
        if (uses->queueOps.empty())
            return;

        auto queueType = mlir::cast<QueueType>(
            mlir::isa<QueuePushOp>(uses->queueOps.front())
                ? mlir::cast<QueuePushOp>(uses->queueOps.front()).getQueue().getType()
                : mlir::cast<QueuePopOp>(uses->queueOps.front()).getQueue().getType());

        const AltImpl *chosen = nullptr;
        for (const AltImpl &impl : altImpls)
            if (impl.elementType == queueType.getElementType())
                chosen = &impl;
        if (!chosen) {
            globalOp.emitRemark("not substituted: no substitute implementation for element type ")
                << queueType.getElementType() << " is instantiated in this module";
            return;
        }

        auto newPtrType = cir::PointerType::get(chosen->recordType);
        auto newDtor = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(globalOp, chosen->dtor);
        auto newTryPop = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(globalOp, chosen->tryPop);

        // A pop lifted with a result slot has to call a try_pop that takes
        // one, and one lifted with a result a try_pop that returns it. Both
        // implementations are compiled for the same target, so they agree;
        // check rather than assume.
        bool substituteReturnsIndirectly = sretArgIndex(newTryPop).has_value();
        for (Operation *op : uses->queueOps) {
            auto popOp = mlir::dyn_cast<QueuePopOp>(op);
            if (popOp && static_cast<bool>(popOp.getResultSlot()) != substituteReturnsIndirectly) {
                globalOp.emitRemark("not substituted: the substitute's try_pop returns its value differently");
                return;
            }
        }

        globalOp.setSymTypeAttr(TypeAttr::get(chosen->recordType));
        globalOp.setInitialValueAttr(cir::ZeroAttr::get(chosen->recordType));
        // The record's own alignment, alignas included, is what ClangIR
        // records in cir.record_layouts; the data layout of the record type
        // does not see alignas, and spsc_queue aligns its cursors to 64.
        uint64_t align = 0;
        if (auto layouts = module->getAttrOfType<DictionaryAttr>("cir.record_layouts"))
            if (auto layout = mlir::dyn_cast_or_null<cir::RecordLayoutAttr>(
                    layouts.get(chosen->recordType.getName().getValue())))
                align = layout.getRecordAlign();
        if (!align)
            align = DataLayout::closest(globalOp).getTypeABIAlignment(chosen->recordType);
        globalOp.setAlignmentAttr(IntegerAttr::get(IntegerType::get(&getContext(), 64), align));

        for (cir::GetGlobalOp getGlobalOp : uses->getGlobals)
            getGlobalOp.getResult().setType(newPtrType);

        for (Operation *op : uses->queueOps) {
            if (auto pushOp = mlir::dyn_cast<QueuePushOp>(op)) {
                pushOp.setFallbackImplAttr(chosen->push);
                pushOp.setSourceQueueTypeAttr(TypeAttr::get(newPtrType));
            } else {
                auto popOp = mlir::cast<QueuePopOp>(op);
                popOp.setFallbackImplAttr(chosen->tryPop);
                popOp.setSourceQueueTypeAttr(TypeAttr::get(newPtrType));
            }
        }

        auto newCtor = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(globalOp, chosen->construct);
        for (cir::CallOp callOp : uses->constructions) {
            callOp.setCalleeAttr(chosen->construct);
            copyArgAttrsFromCallee(callOp, newCtor);
        }

        for (cir::GetGlobalOp dtorRef : uses->dtorRefs) {
            dtorRef->setAttr("name", chosen->dtor);
            dtorRef.getResult().setType(cir::PointerType::get(newDtor.getFunctionType()));
        }

        globalOp.emitRemark("substituted ") << chosen->recordType.getName();
    }

    // [[gnu::used]] puts each anchor in @llvm.used, which is what makes the
    // compiler emit it; take it out again, dropping @llvm.used if nothing
    // else is left in it.
    void dropFromLlvmUsed(ModuleOp module, StringRef symbol) {
        auto used = module.lookupSymbol<cir::GlobalOp>("llvm.used");
        if (!used)
            return;
        auto array = mlir::dyn_cast_or_null<cir::ConstArrayAttr>(used.getInitialValueAttr());
        auto elements = array ? mlir::dyn_cast<ArrayAttr>(array.getElts()) : ArrayAttr();
        if (!elements)
            return;

        llvm::SmallVector<Attribute> kept;
        for (Attribute element : elements) {
            auto view = mlir::dyn_cast<cir::GlobalViewAttr>(element);
            if (!view || view.getSymbol().getValue() != symbol)
                kept.push_back(element);
        }
        if (kept.size() == elements.size())
            return;
        if (kept.empty()) {
            used.erase();
            return;
        }

        auto oldType = mlir::cast<cir::ArrayType>(array.getType());
        auto newType = cir::ArrayType::get(oldType.getElementType(), kept.size());
        used.setSymTypeAttr(TypeAttr::get(newType));
        used.setInitialValueAttr(cir::ConstArrayAttr::get(newType, ArrayAttr::get(&getContext(), kept)));
    }

    // The anchors exist only to make the substitutes' functions appear in
    // the module. Nothing calls them; @llvm.used keeps them alive, and the
    // shim's constructor may take an address it then discards.
    void eraseAnchors(ModuleOp module) {
        for (cir::FuncOp funcOp : llvm::to_vector(module.getOps<cir::FuncOp>())) {
            if (!funcHasAnnotation(funcOp, "wtc_anchor"))
                continue;

            dropFromLlvmUsed(module, funcOp.getSymName());

            if (auto uses = SymbolTable::getSymbolUses(funcOp.getOperation(), module.getOperation())) {
                for (const SymbolTable::SymbolUse &use : llvm::make_early_inc_range(*uses)) {
                    auto getGlobalOp = mlir::dyn_cast<cir::GetGlobalOp>(use.getUser());
                    if (getGlobalOp && getGlobalOp.getResult().use_empty())
                        getGlobalOp.erase();
                }
            }

            if (SymbolTable::symbolKnownUseEmpty(funcOp.getOperation(), module.getOperation()))
                funcOp.erase();
            else
                funcOp.emitRemark("anchor kept: something other than a discarded address refers to it");
        }
    }
};

namespace mlir::wtc {
    void registerSelectQueueImplementationPass() {
        PassRegistration<SelectQueueImplementationPass>();
    }
}
