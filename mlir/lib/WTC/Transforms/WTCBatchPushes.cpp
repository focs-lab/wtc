#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Demangle/Demangle.h"

#include <optional>
#include <string>

using namespace mlir;
using namespace mlir::wtc;

// Synchronisation vectorisation. A loop that pushes to a queue on every
// iteration pays the queue's synchronisation on every iteration: a mutex
// on the general-purpose queue, a release store on a ring. A library sees
// one push at a time and cannot do better; the compiler sees the loop.
// Where the program allows it, the pushes of the loop become one batched
// run (shim/queue.hpp): batch_begin before the loop, push_deferred in
// place of each push, batch_end after it. The elements are published
// every WTC_BATCH_K pushes and at the end, in order.
//
// That changes when consumers see an element, not what they see: each
// still arrives once and in its producer's order, and a push still
// happens before the pop that returns it (decisions/0003). The delay is
// what makes it a choice the program has to make, so only a function
// marked WTC_BATCH (shim/progress.hpp) is touched. Within it, a loop is
// batched only when nothing in it could be waiting for an element it has
// not yet published, and nothing can leave it without passing batch_end:
//
//   - the loop holds exactly one wtc operation, a push, on every iteration
//     (not under a condition or an inner loop, and the loop has no
//     continue), to a queue whose handle the loop does not change;
//   - nothing in the loop, or in what it calls, synchronises: no atomic or
//     volatile access, no fence, no inline assembly, no call into the
//     threading library;
//   - nothing leaves the loop but its end or a break: no return, no goto,
//     and no call that may throw;
//   - every call reaches only code the module can see, or external
//     functions that are nothrow and not in the threading library.
//
// One exception is accepted rather than ruled out: a push allocates, and if
// the allocation fails mid-run, the elements still waiting to be published
// are lost with the exception. A program that survives running out of
// memory in a producer loop should not be marked WTC_BATCH.

namespace {

struct Refusal {
    Operation *at;
    std::string why;
};

bool synchronisingExternal(llvm::StringRef name) {
    return name.starts_with("pthread_") || name.starts_with("sem_") || name.starts_with("thrd_") ||
           name.starts_with("mtx_") || name.starts_with("cnd_") || name.contains("futex") ||
           name == "syscall" || name == "sched_yield";
}

bool synchronises(Operation *op) {
    StringRef n = op->getName().getStringRef();
    if (n.starts_with("cir.atomic") || isa<cir::InlineAsmOp>(op)) return true;
    if (auto ld = dyn_cast<cir::LoadOp>(op)) return ld.getIsVolatile() || ld.getMemOrder();
    if (auto st = dyn_cast<cir::StoreOp>(op)) return st.getIsVolatile() || st.getMemOrder();
    return false;
}

class LegalityChecker {
public:
    // Why a function, called from a loop being batched, is unfit: it could
    // synchronise, throw, or act on a queue. Memoised; a cycle is optimistic,
    // since every operation on it is still checked by its own walk.
    std::optional<Refusal> callee(cir::FuncOp f);
    std::optional<Refusal> call(cir::CallOp call);
    std::optional<Refusal> op(Operation *op);

private:
    llvm::DenseMap<Operation *, std::optional<Refusal>> memo;
};

std::optional<Refusal> LegalityChecker::op(Operation *o) {
    if (synchronises(o)) return Refusal{o, "it synchronises (" + o->getName().getStringRef().str() + ")"};
    if (isa<cir::ThrowOp>(o)) return Refusal{o, "it may throw"};
    if (auto c = dyn_cast<cir::CallOp>(o)) return call(c);
    return std::nullopt;
}

std::optional<Refusal> LegalityChecker::call(cir::CallOp c) {
    cir::FuncOp f = lookupCallee(c);
    if (!f) return Refusal{c, "it makes an indirect call"};
    if (f.getBody().empty()) {
        const std::string name = llvm::demangle(f.getSymName().str());
        if (synchronisingExternal(f.getSymName()))
            return Refusal{c, "it calls into the threading library (" + name + ")"};
        if (!c.getNothrow()) return Refusal{c, "it calls " + name + ", which may throw"};
        return std::nullopt;
    }
    if (auto r = callee(f)) return Refusal{c, "it calls " + llvm::demangle(f.getSymName().str()) + ", where " + r->why};
    return std::nullopt;
}

std::optional<Refusal> LegalityChecker::callee(cir::FuncOp f) {
    if (auto it = memo.find(f); it != memo.end()) return it->second;
    memo[f] = std::nullopt;
    std::optional<Refusal> found;
    f.walk([&](Operation *o) {
        if (isa<QueuePushOp, QueuePopOp, QueueBatchBeginOp, QueuePushDeferredOp, QueueBatchEndOp, LockOp,
                UnlockOp>(o))
            found = Refusal{o, "it acts on a queue or a mutex"};
        else
            found = op(o);
        return found ? WalkResult::interrupt() : WalkResult::advance();
    });
    memo[f] = found;
    return found;
}

// The shim's function for one batch operation on a queue of this type.
cir::FuncOp shimFunction(ModuleOp module, StringRef annotation, Type queuePtr) {
    cir::FuncOp found;
    module.walk([&](cir::FuncOp f) {
        if (found || !hasAnnotation(f, annotation) || f.getNumArguments() == 0) return;
        if (f.getArgumentTypes().front() == queuePtr) found = f;
    });
    return found;
}

class WTCBatchPushesPass : public PassWrapper<WTCBatchPushesPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCBatchPushesPass)

    StringRef getArgument() const final { return "wtc-batch-pushes"; }
    StringRef getDescription() const final {
        return "Batch the pushes of loops in functions marked WTC_BATCH where nothing in the loop can "
               "wait for an unpublished element (run after --lift-cir-to-wtc)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        ModuleOp module = getOperation();
        LegalityChecker legal;
        llvm::SmallVector<std::pair<cir::LoopOpInterface, QueuePushOp>, 4> todo;

        module.walk([&](cir::FuncOp f) {
            if (!hasAnnotation(f, "wtc_batch") || f.getBody().empty()) return;
            f.walk([&](cir::LoopOpInterface loop) {
                // Loops with no push have nothing to batch; loops whose only
                // push sits in an inner loop are left to that loop.
                llvm::SmallVector<Operation *, 2> wtcOps;
                loop->walk([&](Operation *o) {
                    if (isa<WTCDialect>(o->getDialect())) wtcOps.push_back(o);
                });
                if (wtcOps.empty()) return;
                auto push = dyn_cast<QueuePushOp>(wtcOps.front());
                if (push && enclosingLoop(push) != loop.getOperation()) return;
                if (auto r = check(loop, wtcOps, legal)) {
                    InFlightDiagnostic d = loop->emitRemark() << "wtc: pushes in this loop not batched: " << r->why;
                    if (r->at != loop.getOperation()) d.attachNote(r->at->getLoc()) << "here";
                    return;
                }
                todo.push_back({loop, push});
            });
        });
        for (auto &[loop, push] : todo)
            if (failed(rewrite(module, loop, push))) return signalPassFailure();
    }

private:
    static Operation *enclosingLoop(Operation *op) {
        for (Operation *p = op->getParentOp(); p; p = p->getParentOp())
            if (isa<cir::LoopOpInterface>(p)) return p;
        return nullptr;
    }

    // The value the push's handle was cast from, if the loop cannot change
    // it: defined outside the loop, or a global's address taken inside it.
    static bool handleInvariant(cir::LoopOpInterface loop, QueuePushOp push) {
        Value v = push.getQueue();
        if (auto cast = v.getDefiningOp<UnrealizedConversionCastOp>()) v = cast.getInputs().front();
        if (!loop->isAncestor(v.getParentBlock()->getParentOp())) return true;
        return static_cast<bool>(v.getDefiningOp<cir::GetGlobalOp>());
    }

    std::optional<Refusal> check(cir::LoopOpInterface loop, llvm::ArrayRef<Operation *> wtcOps,
                                 LegalityChecker &legal) {
        if (wtcOps.size() != 1 || !isa<QueuePushOp>(wtcOps.front()))
            return Refusal{wtcOps.size() == 1 ? wtcOps.front() : wtcOps[1],
                           "the loop acts on queues or mutexes other than through one push"};
        auto push = cast<QueuePushOp>(wtcOps.front());
        // On every iteration: only scopes between the push and the body.
        for (Operation *p = push->getParentOp(); p != loop.getOperation(); p = p->getParentOp())
            if (!isa<cir::ScopeOp, cir::CleanupScopeOp>(p))
                return Refusal{p, "the push is not made on every iteration"};
        if (!handleInvariant(loop, push))
            return Refusal{push, "the loop may change which queue it pushes to"};
        std::optional<Refusal> found;
        loop->walk([&](Operation *o) {
            if (isa<cir::ContinueOp>(o))
                found = Refusal{o, "a continue skips the push on some iterations"};
            else if (isa<cir::ReturnOp, cir::GotoOp, cir::LabelOp>(o))
                found = Refusal{o, "control can leave the loop other than at its end or a break"};
            else if (!isa<WTCDialect>(o->getDialect()))
                found = legal.op(o);
            return found ? WalkResult::interrupt() : WalkResult::advance();
        });
        return found;
    }

    LogicalResult rewrite(ModuleOp module, cir::LoopOpInterface loop, QueuePushOp push) {
        auto sourceType = push.getSourceQueueTypeAttr();
        if (!sourceType) return push.emitError("wtc: a push to batch carries no source type");
        Type queuePtr = sourceType.getValue();
        cir::FuncOp begin = shimFunction(module, "wtc_queue_batch_begin", queuePtr);
        cir::FuncOp deferred = shimFunction(module, "wtc_queue_push_deferred", queuePtr);
        cir::FuncOp end = shimFunction(module, "wtc_queue_batch_end", queuePtr);
        if (!begin || !deferred || !end || begin.getNumArguments() != 2) {
            loop->emitRemark() << "wtc: pushes in this loop not batched: the shim's batch functions "
                                  "for this queue are not in the module";
            return success();
        }
        MLIRContext *ctx = &getContext();
        auto batchPtr = cast<cir::PointerType>(begin.getArgumentTypes()[1]);

        // The batch lives for the whole function, so its slot goes where
        // ClangIR puts every other: the top of the entry block.
        auto fn = loop->getParentOfType<cir::FuncOp>();
        OpBuilder top(&fn.getBody().front(), fn.getBody().front().begin());
        auto slot = cir::AllocaOp::create(top, loop.getLoc(), batchPtr, "wtc.batch",
                                          top.getI64IntegerAttr(8));

        // The handle outside the loop: the same value if it already is,
        // otherwise the global's address taken again before the loop.
        OpBuilder before(loop.getOperation());
        Value inner = push.getQueue();
        Value outer = inner;
        if (auto cast = inner.getDefiningOp<UnrealizedConversionCastOp>(); cast && loop->isAncestor(cast)) {
            Value src = cast.getInputs().front();
            if (Operation *def = src.getDefiningOp(); def && loop->isAncestor(def)) {
                auto gg = src.getDefiningOp<cir::GetGlobalOp>();
                src = before.clone(*gg)->getResult(0);
            }
            outer = UnrealizedConversionCastOp::create(before, loop.getLoc(), inner.getType(), src).getResult(0);
        }

        auto ref = [&](cir::FuncOp f) { return FlatSymbolRefAttr::get(ctx, f.getSymName()); };
        QueueBatchBeginOp::create(before, loop.getLoc(), outer, slot.getResult(), ref(begin), sourceType,
                                  ArrayAttr(), TopologyAttr(), IntegerAttr(), ImplAttr());
        OpBuilder at(push);
        QueuePushDeferredOp::create(at, push.getLoc(), inner, slot.getResult(), push.getValue(), ref(deferred),
                                    sourceType, ArrayAttr(), TopologyAttr(), IntegerAttr(), ImplAttr());
        push.erase();
        OpBuilder after(loop->getBlock(), std::next(Block::iterator(loop.getOperation())));
        QueueBatchEndOp::create(after, loop.getLoc(), outer, slot.getResult(), ref(end), sourceType,
                                ArrayAttr(), TopologyAttr(), IntegerAttr(), ImplAttr());
        loop->emitRemark() << "wtc: pushes in this loop batched: published every " << kBatchNote
                           << " pushes and at the end";
        return success();
    }

    static constexpr const char *kBatchNote = "WTC_BATCH_K (64)";
};

} // namespace

namespace mlir::wtc {
void registerWTCBatchPushesPass() { PassRegistration<WTCBatchPushesPass>(); }
} // namespace mlir::wtc
