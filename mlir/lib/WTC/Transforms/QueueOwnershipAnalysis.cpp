#include "WTC/WTCOps.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/Pass/Pass.h"
#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <string>

using namespace mlir;
using namespace mlir::wtc;

// A thread the analysis knows about, identified by the static site that
// starts it: a spawn call, or the program entry for main.
//
// `many` means the site may run more than once, so the one id stands for an
// unknown number of dynamic threads and counts as at least two. `unknown`
// is the single thread standing for every context the analysis cannot see
// the start of: code whose address escapes, and code nothing here calls.
struct ThreadInfo {
    std::string entry;
    bool many = false;
    bool unknown = false;
};

// One way a function is entered on a given thread. Parameter i of the
// function is bound to argument `offset + i` of `site`: a spawn binds its
// context argument to the entry's first parameter, an ordinary call binds
// argument i to parameter i. `site` is null for entries that bind nothing,
// main and the unknown context.
struct ThreadInstance {
    unsigned threadId;
    cir::CallOp site;
    unsigned offset;

    bool operator==(const ThreadInstance &other) const {
        return threadId == other.threadId && site == other.site && offset == other.offset;
    }
};

struct QueueAccessInfo {
    llvm::SmallSetVector<unsigned, 4> producers;
    llvm::SmallSetVector<unsigned, 4> consumers;
};

// A verdict as three facts, so that joining two is a bitwise or: an op
// reaching one SPSC and one MPSC queue is MPSC, one reaching an MPSC and an
// SPMC queue is MPMC, and anything joined with UNKNOWN is UNKNOWN.
struct QueueKind {
    bool multiProducer = false;
    bool multiConsumer = false;
    bool unknown = false;

    QueueKind join(const QueueKind &other) const {
        return {multiProducer || other.multiProducer,
                multiConsumer || other.multiConsumer,
                unknown || other.unknown};
    }

    StringRef str() const {
        if (unknown)
            return "UNKNOWN";
        if (multiProducer)
            return multiConsumer ? "MPMC" : "MPSC";
        return multiConsumer ? "SPMC" : "SPSC";
    }
};

struct SpawnSite {
    cir::FuncOp entry;
    // Index into the call's arguments of the context argument, if any.
    unsigned contextIndex;
};

struct QueueOwnershipAnalysisPass
    : public PassWrapper<QueueOwnershipAnalysisPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(QueueOwnershipAnalysisPass)

    ModuleOp module;

    llvm::SmallVector<ThreadInfo> threads;
    std::optional<unsigned> unknownThreadId;
    llvm::DenseMap<Operation *, llvm::SmallVector<ThreadInstance>> instances;
    llvm::DenseMap<Operation *, bool> runsAtMostOnceCache;
    // Results are printed in the order queues are first accessed in the IR,
    // so output is stable from run to run.
    llvm::MapVector<Operation *, QueueAccessInfo> queueAccesses;
    // Set when some access could not be attributed to a queue: it may touch
    // any of them, so no verdict can be trusted.
    bool unattributedAccess = false;
    // Every queue each push and pop can reach, and the ops for which some
    // access reached no queue the analysis could name.
    llvm::MapVector<Operation *, llvm::SetVector<Operation *>> opRoots;
    llvm::SmallPtrSet<Operation *, 4> opsWithUnresolvedAccess;

    StringRef getArgument() const final {
        return "queue-ownership-analysis";
    }

    void runOnOperation() override {
        module = getOperation();

        processThreadSpawn();
        computeThreadInstances();
        processQueueOp();
        classifyQueues();

        markAllAnalysesPreserved();
    }

    cir::FuncOp lookupFunc(Operation *from, FlatSymbolRefAttr sym) {
        return SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(from, sym);
    }

    bool isSpawnCall(cir::CallOp callOp) {
        auto calleeAttr = callOp.getCalleeAttr();
        if (!calleeAttr)
            return false;
        return getSemanticKind(lookupFunc(callOp, calleeAttr)) == SemanticOpKind::ThreadSpawn;
    }

    // The entry of a spawn is the first argument that is the address of a
    // cir.func, not operand zero: a spawn returning std::thread by value
    // passes the result slot as a hidden first argument. The context
    // argument, if there is one, follows the entry.
    std::optional<SpawnSite> decodeSpawn(cir::CallOp callOp) {
        if (!isSpawnCall(callOp))
            return std::nullopt;

        for (auto [i, arg] : llvm::enumerate(callOp.getArgOperands())) {
            auto getGlobalOp = traceToLocalRoot(arg).getDefiningOp<cir::GetGlobalOp>();
            if (!getGlobalOp)
                continue;
            auto nameAttr = getGlobalOp->getAttrOfType<FlatSymbolRefAttr>("name");
            if (!nameAttr)
                continue;
            if (auto entry = lookupFunc(callOp, nameAttr))
                return SpawnSite{entry, static_cast<unsigned>(i) + 1};
        }
        return std::nullopt;
    }

    void processThreadSpawn() {
        module.walk([&](cir::CallOp callOp) {
            std::optional<SpawnSite> spawn = decodeSpawn(callOp);
            if (!spawn)
                return;

            unsigned threadId = threads.size();
            threads.push_back({spawn->entry.getSymName().str(), !executesAtMostOnce(callOp), false});
            addInstance(spawn->entry, {threadId, callOp, spawn->contextIndex});

            llvm::outs()
                << "THREAD T"
                << threadId
                << "\n  entry: "
                << spawn->entry.getSymName()
                << (threads[threadId].many ? "\n  started: possibly more than once" : "")
                << "\n";
        });

        if (auto mainFunc = module.lookupSymbol<cir::FuncOp>("main")) {
            unsigned threadId = threads.size();
            threads.push_back({"main", false, false});
            addInstance(mainFunc, {threadId, cir::CallOp(), 0});

            llvm::outs() << "THREAD T" << threadId << "\n  entry: main\n";
        }
    }

    bool addInstance(cir::FuncOp func, ThreadInstance inst) {
        auto &list = instances[func.getOperation()];
        if (llvm::is_contained(list, inst))
            return false;
        list.push_back(inst);
        return true;
    }

    unsigned getUnknownThread() {
        if (!unknownThreadId) {
            unknownThreadId = threads.size();
            threads.push_back({"<unknown>", true, true});
        }
        return *unknownThreadId;
    }

    // Whether every use of a function's address is as the entry of a spawn
    // the analysis has decoded. Any other use -- stored, passed elsewhere,
    // called indirectly -- means it may run on threads this module cannot
    // see the start of.
    bool addressEscapes(cir::GetGlobalOp getGlobalOp, cir::FuncOp func,
                        unsigned *spawnUses = nullptr) {
        llvm::SmallVector<Value> worklist{getGlobalOp.getResult()};
        while (!worklist.empty()) {
            Value v = worklist.pop_back_val();
            for (Operation *user : v.getUsers()) {
                if (mlir::isa<cir::CastOp>(user) ||
                    (mlir::isa<mlir::UnrealizedConversionCastOp>(user) && user->getNumOperands() == 1)) {
                    worklist.push_back(user->getResult(0));
                    continue;
                }
                auto callOp = mlir::dyn_cast<cir::CallOp>(user);
                std::optional<SpawnSite> spawn = callOp ? decodeSpawn(callOp) : std::nullopt;
                if (!spawn || spawn->entry != func)
                    return true;
                if (spawnUses)
                    ++*spawnUses;
            }
        }
        return false;
    }

    // Seeds every function the analysis can see the start of, then follows
    // direct calls until nothing changes. Instances only accumulate, so the
    // result does not depend on the order functions are visited in.
    void computeThreadInstances() {
        module.walk([&](cir::FuncOp func) {
            if (func.isDeclaration() || func.getSymName() == "main")
                return;

            bool hasCaller = false;
            bool escapes = false;
            if (auto uses = SymbolTable::getSymbolUses(func.getOperation(), module.getOperation())) {
                for (const SymbolTable::SymbolUse &use : *uses) {
                    if (auto getGlobalOp = mlir::dyn_cast<cir::GetGlobalOp>(use.getUser())) {
                        escapes |= addressEscapes(getGlobalOp, func);
                        hasCaller = true;
                    } else if (mlir::isa<cir::CallOp>(use.getUser())) {
                        hasCaller = true;
                    } else {
                        escapes = true;
                    }
                }
            }

            if (escapes || !hasCaller)
                addInstance(func, {getUnknownThread(), cir::CallOp(), 0});
        });

        llvm::SetVector<Operation *> worklist;
        for (auto &entry : instances)
            worklist.insert(entry.first);

        while (!worklist.empty()) {
            auto func = mlir::cast<cir::FuncOp>(worklist.pop_back_val());
            llvm::SmallVector<ThreadInstance> callerInstances = instances[func.getOperation()];

            func.walk([&](cir::CallOp callOp) {
                auto calleeAttr = callOp.getCalleeAttr();
                if (!calleeAttr)
                    return;
                cir::FuncOp callee = lookupFunc(callOp, calleeAttr);
                if (!callee)
                    return;

                bool changed = false;
                for (const ThreadInstance &callerInst : callerInstances)
                    changed |= addInstance(callee, {callerInst.threadId, callOp, 0});
                if (changed)
                    worklist.insert(callee.getOperation());
            });
        }
    }

    static bool blockIsInCycle(Block *block) {
        llvm::SmallVector<Block *> worklist(block->getSuccessors().begin(),
                                            block->getSuccessors().end());
        llvm::SmallPtrSet<Block *, 8> seen;
        while (!worklist.empty()) {
            Block *b = worklist.pop_back_val();
            if (b == block)
                return true;
            if (!seen.insert(b).second)
                continue;
            worklist.append(b->getSuccessors().begin(), b->getSuccessors().end());
        }
        return false;
    }

    // Whether op can run at most once in the whole execution. False is
    // always a safe answer: it only ever makes a thread count as several.
    bool executesAtMostOnce(Operation *op) {
        Operation *current = op;
        while (current && !mlir::isa<cir::FuncOp>(current)) {
            if (mlir::isa<cir::LoopOpInterface>(current) && current != op)
                return false;
            if (Block *block = current->getBlock(); block && blockIsInCycle(block))
                return false;
            current = current->getParentOp();
        }

        auto func = mlir::dyn_cast_or_null<cir::FuncOp>(current);
        return func && functionRunsAtMostOnce(func);
    }

    bool functionRunsAtMostOnce(cir::FuncOp func) {
        Operation *key = func.getOperation();
        if (auto it = runsAtMostOnceCache.find(key); it != runsAtMostOnceCache.end())
            return it->second;

        // A function reached again while its own answer is being computed is
        // recursive, and so runs more than once. Caching that answer cannot
        // mislead later queries: everything derived from it is also false.
        runsAtMostOnceCache[key] = false;

        auto uses = SymbolTable::getSymbolUses(key, module.getOperation());
        bool noUses = !uses || uses->empty();
        bool result = false;

        if (func.getSymName() == "main") {
            result = noUses;
        } else if (!noUses && llvm::hasSingleElement(*uses)) {
            Operation *user = uses->begin()->getUser();
            if (auto callOp = mlir::dyn_cast<cir::CallOp>(user)) {
                result = executesAtMostOnce(callOp);
            } else if (auto getGlobalOp = mlir::dyn_cast<cir::GetGlobalOp>(user)) {
                unsigned spawnUses = 0;
                if (!addressEscapes(getGlobalOp, func, &spawnUses) && spawnUses == 1) {
                    for (Operation *spawnUser : getGlobalOp.getResult().getUsers())
                        if (auto spawnCall = mlir::dyn_cast<cir::CallOp>(spawnUser))
                            result = executesAtMostOnce(spawnCall);
                }
            }
        }

        runsAtMostOnceCache[key] = result;
        return result;
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

    // Every object v can refer to on the given thread. A parameter is
    // followed through each way its function is entered on that thread,
    // using the argument at that parameter's own position, so a function
    // taking two queues keeps them apart and a function entered twice with
    // two queues reaches both.
    void resolveQueueRoots(Value v, unsigned threadId,
                           llvm::DenseSet<Value> &visited,
                           llvm::SetVector<Operation *> &roots) {
        if (!visited.insert(v).second)
            return;

        Value resolved = traceToLocalRoot(v);

        if (auto blockArg = mlir::dyn_cast<BlockArgument>(resolved)) {
            cir::FuncOp funcOp = findEnclosingFunc(blockArg.getOwner());
            if (!funcOp || blockArg.getOwner() != &funcOp.getBody().front())
                return;

            for (const ThreadInstance &inst : instances.lookup(funcOp.getOperation())) {
                if (inst.threadId != threadId || !inst.site)
                    continue;
                unsigned index = inst.offset + blockArg.getArgNumber();
                cir::CallOp site = inst.site;
                auto args = site.getArgOperands();
                if (index < args.size())
                    resolveQueueRoots(args[index], threadId, visited, roots);
            }
            return;
        }

        Operation *root = resolved.getDefiningOp();
        if (auto getGlobalOp = mlir::dyn_cast_or_null<cir::GetGlobalOp>(root)) {
            if (auto nameAttr = getGlobalOp->getAttrOfType<FlatSymbolRefAttr>("name")) {
                if (auto globalOp = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(
                        getGlobalOp.getOperation(), nameAttr))
                    root = globalOp.getOperation();
            }
        }
        if (root)
            roots.insert(root);
    }

    void recordQueueAccess(Operation *op, Value queueValue, bool isPush) {
        auto caller = op->getParentOfType<cir::FuncOp>();

        // Thread ids are handed out in IR order, so visiting them sorted keeps
        // the report in IR order too, whatever order the fixed point found
        // the instances in.
        llvm::SmallVector<unsigned, 4> threadIds;
        for (const ThreadInstance &inst : instances.lookup(caller.getOperation()))
            threadIds.push_back(inst.threadId);
        llvm::sort(threadIds);
        threadIds.erase(std::unique(threadIds.begin(), threadIds.end()), threadIds.end());

        if (threadIds.empty()) {
            llvm::outs()
                << "Could not resolve thread context for "
                << caller.getSymName()
                << "\n";
            unattributedAccess = true;
            opsWithUnresolvedAccess.insert(op);
            return;
        }

        for (unsigned threadId : threadIds) {
            if (threads[threadId].unknown)
                llvm::outs()
                    << "Could not resolve thread context for "
                    << caller.getSymName()
                    << "\n";

            llvm::DenseSet<Value> visited;
            llvm::SetVector<Operation *> roots;
            resolveQueueRoots(queueValue, threadId, visited, roots);

            if (roots.empty()) {
                llvm::outs()
                    << "Could not resolve queue instance for T"
                    << threadId
                    << "\n";
                unattributedAccess = true;
                opsWithUnresolvedAccess.insert(op);
                continue;
            }

            for (Operation *root : roots) {
                opRoots[op].insert(root);
                QueueAccessInfo &info = queueAccesses[root];
                if (isPush)
                    info.producers.insert(threadId);
                else
                    info.consumers.insert(threadId);
            }

            llvm::outs()
                << (isPush ? "QUEUE PUSH" : "QUEUE POP")
                << "\n  caller: "
                << caller.getSymName()
                << "\n  thread: T"
                << threadId
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

    struct SideSummary {
        size_t threads = 0;
        bool many = false;
        bool unknown = false;
    };

    SideSummary summarise(const llvm::SmallSetVector<unsigned, 4> &threadIds) {
        SideSummary summary;
        for (unsigned threadId : threadIds) {
            ++summary.threads;
            summary.many |= threads[threadId].many;
            summary.unknown |= threads[threadId].unknown;
        }
        return summary;
    }

    static void printSide(StringRef name, const SideSummary &side) {
        llvm::outs() << "  " << name << ": ";
        if (side.many)
            llvm::outs() << "many";
        else
            llvm::outs() << side.threads;
        llvm::outs() << "\n";
    }

    void classifyQueues() {
        if (unattributedAccess && !queueAccesses.empty())
            llvm::outs()
                << "Some access could not be attributed to a queue; "
                   "every queue is classified UNKNOWN\n";

        llvm::DenseMap<Operation *, QueueKind> rootKinds;

        for (auto &[root, info] : queueAccesses) {
            SideSummary producers = summarise(info.producers);
            SideSummary consumers = summarise(info.consumers);

            llvm::outs() << "Queue ";
            printQueueLabel(root);
            llvm::outs() << "\n";
            printSide("producers", producers);
            printSide("consumers", consumers);

            QueueKind kind;
            kind.multiProducer = producers.threads > 1 || producers.many;
            kind.multiConsumer = consumers.threads > 1 || consumers.many;
            // A side nobody uses gives nothing to specialise for, and a
            // queue reached from an unknown context could have any number
            // of users on either side.
            kind.unknown = unattributedAccess
                || producers.unknown || consumers.unknown
                || producers.threads == 0 || consumers.threads == 0;
            rootKinds[root] = kind;

            llvm::outs() << "  classification: " << kind.str() << "\n";
        }

        stampVerdicts(rootKinds);
    }

    // Writes the verdicts into the IR, so a later pass can read them
    // instead of re-deriving them: each queue's root carries its own
    // verdict, and each push or pop carries the join of the verdicts of
    // every queue it can reach.
    void stampVerdicts(const llvm::DenseMap<Operation *, QueueKind> &rootKinds) {
        MLIRContext *context = &getContext();
        auto stamp = [&](Operation *op, const QueueKind &kind) {
            op->setAttr(kQueueKindAttrName, StringAttr::get(context, kind.str()));
        };

        for (auto &[root, kind] : rootKinds)
            stamp(root, kind);

        QueueKind unknown;
        unknown.unknown = true;

        for (auto &[op, roots] : opRoots) {
            QueueKind kind;
            for (Operation *root : roots)
                kind = kind.join(rootKinds.lookup(root));
            if (opsWithUnresolvedAccess.contains(op))
                kind = kind.join(unknown);
            stamp(op, kind);
        }
        for (Operation *op : opsWithUnresolvedAccess)
            if (!opRoots.contains(op))
                stamp(op, unknown);
    }
};

namespace mlir::wtc {
    void registerQueueOwnershipAnalysisPass() {
        PassRegistration<QueueOwnershipAnalysisPass>();
    }
}
