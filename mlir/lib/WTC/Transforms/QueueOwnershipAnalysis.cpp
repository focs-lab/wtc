#include "WTC/WTCOps.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"

#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/DenseMap.h"

using namespace mlir;
using namespace mlir::wtc;

struct QueueAccessInfo {
    llvm::SmallDenseSet<unsigned, 4> producers;
    llvm::SmallDenseSet<unsigned, 4> consumers;
    llvm::SmallPtrSet<Operation *, 4> pushOps;
    llvm::SmallPtrSet<Operation *, 4> popOps;
};

struct ThreadInstance {
    unsigned threadId;
    Value boundArg;
};

struct QueueOwnershipAnalysisPass
    : public PassWrapper<QueueOwnershipAnalysisPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(QueueOwnershipAnalysisPass)

    unsigned nextThreadId = 0;
    ModuleOp module;

    llvm::DenseMap<Operation *, llvm::SmallVector<ThreadInstance>> threadContexts;
    llvm::DenseMap<Operation *, llvm::SmallVector<ThreadInstance>> threadInstanceCache;
    llvm::DenseMap<Operation *, QueueAccessInfo> queueAccesses;
    llvm::DenseMap<std::pair<Value, unsigned>, Operation *> rootCache;

    StringRef getArgument() const final {
        return "queue-ownership-analysis";
    }

    void runOnOperation() override {
        module = getOperation();

        processThreadSpawn();
        processQueueOp();
        classifyQueues();
    }

    void processThreadSpawn() {
        module.walk([&](cir::CallOp callOp) {
            auto calleeAttr = callOp.getCalleeAttr();
            if (!calleeAttr)
                return;

            auto calleeFunc =
                SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(
                    callOp.getOperation(), calleeAttr);
            if (!calleeFunc)
                return;

            SemanticOpKind semanticOp = getSemanticKind(calleeFunc);
            if (semanticOp != SemanticOpKind::ThreadSpawn)
                return;

            if (callOp.getNumOperands() == 0)
                return;

            auto getGlobalOp = callOp.getOperand(0).getDefiningOp<cir::GetGlobalOp>();
            if (!getGlobalOp)
                return;

            auto nameAttr = getGlobalOp->getAttrOfType<FlatSymbolRefAttr>("name");
            if (!nameAttr)
                return;

            auto entryFunc =
                SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(
                    callOp.getOperation(), nameAttr);
            if (!entryFunc)
                return;

            Value boundArg;
            if (callOp.getNumOperands() == 2)
                boundArg = callOp.getOperand(1);

            unsigned threadId = nextThreadId++;
            threadContexts[entryFunc.getOperation()].push_back({threadId, boundArg});

            llvm::outs()
                << "THREAD T"
                << threadId
                << "\n  entry: "
                << entryFunc.getSymName()
                << "\n";
        });
    }

    void processQueueOp() {
        module.walk([&](QueuePushOp pushOp) {
            recordQueueAccess(pushOp.getOperation(), pushOp.getQueue(), /*isPush=*/true);
        });
        module.walk([&](QueuePopOp popOp) {
            recordQueueAccess(popOp.getOperation(), popOp.getQueue(), /*isPush=*/false);
        });
    }

    static cir::FuncOp findEnclosingFunc(Block *block) {
        Operation *op = block->getParentOp();
        if (!op)
            return nullptr;
        if (auto funcOp = mlir::dyn_cast<cir::FuncOp>(op))
            return funcOp;
        return op->getParentOfType<cir::FuncOp>();
    }

    llvm::SmallVector<ThreadInstance> getThreadInstances(cir::FuncOp func) {
        if (auto it = threadInstanceCache.find(func.getOperation()); it != threadInstanceCache.end())
            return it->second;

        threadInstanceCache[func.getOperation()] = {};

        llvm::SmallVector<ThreadInstance> collected;
        if (auto it = threadContexts.find(func.getOperation()); it != threadContexts.end())
            collected.append(it->second.begin(), it->second.end());

        if (auto uses = SymbolTable::getSymbolUses(func.getOperation(), module.getOperation())) {
            for (const SymbolTable::SymbolUse &use : *uses) {
                auto callOp = mlir::dyn_cast<cir::CallOp>(use.getUser());
                if (!callOp)
                    continue;

                auto calleeAttr = callOp.getCalleeAttr();
                if (!calleeAttr)
                    continue;

                auto calleeFunc = SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(
                    callOp.getOperation(), calleeAttr);
                if (calleeFunc != func)
                    continue;

                auto callerFunc = callOp->getParentOfType<cir::FuncOp>();
                if (!callerFunc)
                    continue;

                for (ThreadInstance callerInst : getThreadInstances(callerFunc)) {
                    Value boundArg = callOp.getNumOperands() >= 1 ? callOp.getOperand(0) : Value();
                    collected.push_back({callerInst.threadId, boundArg});
                }
            }
        }

        threadInstanceCache[func.getOperation()] = collected;
        return collected;
    }

    Operation *resolveQueueRoot(Value v, unsigned threadId) {
        auto key = std::make_pair(v, threadId);
        if (auto it = rootCache.find(key); it != rootCache.end())
            return it->second;

        rootCache[key] = nullptr;

        Value resolved = traceToLocalRoot(v);
        Operation *result = nullptr;

        if (auto blockArg = mlir::dyn_cast<BlockArgument>(resolved)) {
            cir::FuncOp funcOp = findEnclosingFunc(blockArg.getOwner());
            if (funcOp && llvm::is_contained(funcOp.getArguments(), blockArg)) {
                for (ThreadInstance inst : getThreadInstances(funcOp)) {
                    if (inst.threadId == threadId && inst.boundArg) {
                        result = resolveQueueRoot(inst.boundArg, threadId);
                        break;
                    }
                }
            }
        } else {
            result = resolved.getDefiningOp();

            if (auto getGlobalOp = mlir::dyn_cast_or_null<cir::GetGlobalOp>(result)) {
                if (auto nameAttr = getGlobalOp->getAttrOfType<FlatSymbolRefAttr>("name")) {
                    if (auto globalOp = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(
                            getGlobalOp.getOperation(), nameAttr))
                        result = globalOp.getOperation();
                }
            }
        }

        rootCache[key] = result;
        return result;
    }

    void recordQueueAccess(Operation *op, Value queueValue, bool isPush) {
        auto caller = op->getParentOfType<cir::FuncOp>();

        llvm::SmallVector<ThreadInstance> instances = getThreadInstances(caller);
        if (instances.empty()) {
            llvm::outs()
                << "Could not resolve thread context for "
                << caller.getSymName()
                << "\n";
            return;
        }

        for (ThreadInstance &inst : instances) {
            Operation *root = resolveQueueRoot(queueValue, inst.threadId);
            if (!root) {
                llvm::outs()
                    << "Could not resolve queue instance for T"
                    << inst.threadId
                    << "\n";
                continue;
            }

            QueueAccessInfo &info = queueAccesses[root];
            if (isPush) {
                info.producers.insert(inst.threadId);
                info.pushOps.insert(op);
            } else {
                info.consumers.insert(inst.threadId);
                info.popOps.insert(op);
            }

            llvm::outs()
                << (isPush ? "QUEUE PUSH" : "QUEUE POP")
                << "\n  caller: "
                << caller.getSymName()
                << "\n  thread: T"
                << inst.threadId
                << "\n";
        }
    }

    static void printQueueLabel(Operation *root) {
        if (auto globalOp = mlir::dyn_cast<cir::GlobalOp>(root)) {
            llvm::outs() << "@" << globalOp.getSymName();
            return;
        }
        llvm::outs() << root->getLoc();
    }

    void classifyQueues() {
        for (auto& [root, info] : queueAccesses) {
            size_t producerCount = info.producers.size();
            size_t consumerCount = info.consumers.size();

            llvm::outs() << "Queue ";
            printQueueLabel(root);
            llvm::outs()
                << "\n  producers: " << producerCount << "\n"
                << "  consumers: " << consumerCount << "\n";

            StringRef classification;
            if (producerCount == 1 && consumerCount == 1) {
                classification = "SPSC";
            } else if (producerCount > 1 && consumerCount == 1) {
                classification = "MPSC";
            } else if (producerCount == 1 && consumerCount > 1) {
                classification = "SPMC";
            } else if (producerCount > 1 && consumerCount > 1) {
                classification = "MPMC";
            } else {
                classification = "UNKNOWN";
            }

            llvm::outs() << "  classification: " << classification << "\n";

            auto kindAttr = StringAttr::get(&getContext(), classification);
            for (Operation *pushOp : info.pushOps)
                pushOp->setAttr(kQueueKindAttrName, kindAttr);
            for (Operation *popOp : info.popOps)
                popOp->setAttr(kQueueKindAttrName, kindAttr);
        }
    }
};

namespace mlir::wtc {
    void registerQueueOwnershipAnalysisPass() {
        PassRegistration<QueueOwnershipAnalysisPass>();
    }
}