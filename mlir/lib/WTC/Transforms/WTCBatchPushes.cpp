#include "WTC/Transforms/Blocking.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/ThreadInstances.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/Transforms/WorkEstimate.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Demangle/Demangle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
//     volatile access, no fence, no inline assembly but prefetches and
//     no-ops without a memory clobber, and nothing the shared answer to
//     "may this wait" (Transforms/Blocking.h) says may wait;
//   - nothing leaves the loop but its end or a break: no return, no goto,
//     and no call that may throw;
//   - every call reaches only code the module can see, through a pointer
//     whose targets are all known if need be, or external functions known
//     not to wait: the allocator (unless the program registers code for it
//     to run), sleeping, and those that only touch the memory they are
//     given. Any other external function is refused, nothrow or not.
//
// The cost is latency: an element waits for up to WTC_BATCH_K - 1 later
// pushes, and so for up to WTC_BATCH_K times the work the loop does
// between two pushes, before a consumer can see it.
//
// One exception is accepted rather than ruled out: a push allocates, and if
// the allocation fails mid-run, the elements still waiting to be published
// are lost with the exception. A program that survives running out of
// memory in a producer loop should not be marked WTC_BATCH.
//
// The same reasoning batches critical sections (--wtc-batch-locks, below):
// a loop that takes and lets go of one mutex on every iteration pays for
// both each time, and in a function marked WTC_BATCH the compiler holds the
// mutex across the loop instead, letting it go every WTC_LOCK_BATCH_K
// sections and at the end (shim/mutex.hpp). Every interleaving of the
// result is one the original allows -- other threads only happen not to
// take the mutex in between -- so no critical section computes anything
// new. What can change is progress: code between the sections now runs
// with the mutex held, and if it waited for a thread that needs the mutex
// the two would wait for each other. The rules are therefore stricter:
//
//   - the loop holds exactly two wtc operations, a lock and an unlock of
//     one mutex whose handle the loop does not change, in one block, the
//     lock first, on every iteration;
//   - nothing in the loop synchronises or throws, as above, and every
//     function it calls without a body here is one that cannot wait for
//     another thread by the shared answer: the allocator (unless the
//     program registers code for it to run), or one that only touches the
//     memory it is given;
//   - nothing leaves the loop but its end or a break outside the critical
//     section: no continue, return or goto, and no break between the lock
//     and the unlock, which would leave with the mutex held in the
//     original and let it go twice here.
//
// Legal is not profitable. The code between two sections runs with the
// mutex held once the loop is batched, so the other threads wait through
// it: W8 found batching 11-12 times faster with nothing between the
// sections and up to 2.9 times slower with 500 ns there. A legal loop is
// therefore batched only when the work between two sections -- one
// iteration without its critical section, estimated over the CIR
// (Transforms/WorkEstimate.h) and turned into nanoseconds by a constant for
// the class of host -- is under the threshold W8 measured on that class for
// the number of threads that take the mutex, when the thread walk can count
// them, and under the lowest of the class's thresholds otherwise; or when
// one thread alone takes it. The remark gives the estimate and the
// threshold either way, and cost=off batches every legal loop, as before
// (decisions/0006). WTC_BATCH stays the licence: the estimate can only take
// a loop away from batching, never add one.

namespace {

struct Refusal {
    Operation *at;
    std::string why;
};

// Whether an operation synchronises: an atomic or volatile access, a
// fence, or inline assembly other than prefetches and no-ops that take no
// memory clobber. A loop that synchronises may be waiting for an element it
// has not yet published, which is a wait the shared facts cannot see.
bool synchronises(Operation *op) {
    StringRef n = op->getName().getStringRef();
    if (n.starts_with("cir.atomic")) return true;
    if (auto asmOp = dyn_cast<cir::InlineAsmOp>(op))
        return asmOp.getConstraints().contains("~{memory}") ||
               !onlyNonBlockingInstructions(asmOp.getAsmString(), /*inert=*/true);
    if (auto ld = dyn_cast<cir::LoadOp>(op)) return ld.getIsVolatile() || ld.getMemOrder();
    if (auto st = dyn_cast<cir::StoreOp>(op)) return st.getIsVolatile() || st.getMemOrder();
    return false;
}

// Why code a loop being batched runs, or calls, is unfit: the shared
// answer to whether it may wait (Transforms/Blocking.h), with what each
// pass accepts, and two refusals of the batching's own -- it synchronises,
// or a function it calls acts on a queue or a mutex.
//
// Both accept the allocator, as a leaf: it waits for no thread of the
// program and runs none of its code, unless the module registers code for
// it to run (allocatorCallbackRegistration), and then neither accepts it.
// Batched pushes accept sleeping too, which waits for no thread. Neither
// accepts a function without a body here that is not known: nothing
// vouches that it does not wait (semop, a read from a pipe, a join), and
// whether it is nothrow says nothing about that, least of all under
// -fno-exceptions, where every call is. Input and output is refused as
// well, since a write to a pipe or a socket waits for its reader, which may
// be a consumer of the queue.
class LegalityChecker {
public:
    explicit LegalityChecker(ModuleOp module, bool strict = false)
        : registration(allocatorCallbackRegistration(module)),
          query(module, accepted(strict, registration.has_value()),
                [](Operation *o, bool inCallee) -> std::optional<BlockingSite> {
                    auto refuse = [&](std::string why) {
                        return BlockingSite{o, BlockingKind::Refused, std::move(why), cir::FuncOp(), false, {}, {}};
                    };
                    if (inCallee && isa<WTCDialect>(o->getDialect())) return refuse("it acts on a queue or a mutex");
                    if (synchronises(o)) return refuse("it synchronises (" + o->getName().getStringRef().str() + ")");
                    return std::nullopt;
                }) {}

    // What the module registers for the allocator to run, if anything.
    const std::optional<std::string> &allocatorCallback() const { return registration; }

    std::optional<Refusal> op(Operation *o) {
        auto s = query.at(o);
        if (!s) return std::nullopt;
        return Refusal{s->chain.empty() ? s->leaf : s->chain.front(), words(*s)};
    }

private:
    std::optional<std::string> registration;
    BlockingQuery query;

    static BlockingKinds accepted(bool strict, bool allocatorRunsCode) {
        if (allocatorRunsCode) return strict ? BlockingKinds{} : BlockingKinds{BlockingKind::Sleeps};
        return strict ? BlockingKinds{BlockingKind::Allocates}
                      : BlockingKinds{BlockingKind::Allocates, BlockingKind::Sleeps};
    }

    std::string words(const BlockingSite &s) const {
        cir::FuncOp callee = s.callee;
        const std::string name = callee ? llvm::demangle(callee.getSymName().str()) : std::string();
        std::string why;
        switch (s.kind) {
        case BlockingKind::Refused: why = s.why; break;
        case BlockingKind::Throws: why = "it may throw"; break;
        case BlockingKind::MayThrow: why = "it calls " + name + ", which may throw"; break;
        case BlockingKind::UnknownTarget: why = "it makes an indirect call"; break;
        case BlockingKind::Allocates:
            if (registration) {
                why = "it calls " + (callee ? name : std::string("the allocator")) +
                      ", and the program registers code for the allocator to run (" + *registration + ")";
                break;
            }
            [[fallthrough]];
        default:
            if (s.callee && s.threading) why = "it calls into the threading library (" + name + ")";
            else if (s.callee) why = "it calls " + name + ", which may wait for another thread";
            else why = "it " + s.why;
        }
        for (cir::FuncOp f : llvm::reverse(s.via))
            why = "it calls " + llvm::demangle(f.getSymName().str()) + ", where " + why;
        return why;
    }
};

// Whether a function's last parameter is an rvalue reference: push and
// push_deferred each come in a copying and a moving overload, which CIR
// types alike (both are pointers), so the mangled name is what tells them
// apart. A push by rvalue must be batched by the moving overload, or the
// deferred push would copy from an object the program has given up.
bool takesRvalue(cir::FuncOp f) {
    llvm::ItaniumPartialDemangler d;
    std::string mangled = f.getSymName().str();
    if (d.partialDemangle(mangled.c_str())) return false;
    char *params = d.getFunctionParameters(nullptr, nullptr);
    if (!params) return false;
    std::string p(params);
    std::free(params);
    return p.size() >= 3 && p.compare(p.size() - 3, 3, "&&)") == 0;
}

// The shim's function for one batch operation on a queue of this type; for
// push_deferred, the overload whose value parameter matches the push's.
cir::FuncOp shimFunction(ModuleOp module, StringRef annotation, Type queuePtr,
                         std::optional<bool> rvalue = std::nullopt) {
    cir::FuncOp found;
    module.walk([&](cir::FuncOp f) {
        if (found || !hasAnnotation(f, annotation) || f.getNumArguments() == 0) return;
        if (f.getArgumentTypes().front() != queuePtr) return;
        if (rvalue && takesRvalue(f) != *rvalue) return;
        found = f;
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
        LegalityChecker legal(module);
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
        // A push allocates when the queue grows, and the allocator may then
        // run what the program registered for it.
        if (const auto &reg = legal.allocatorCallback())
            return Refusal{push, "a push may allocate, and the program registers code for the allocator to run (" +
                                     *reg + ")"};
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
        cir::FuncOp pushFn = push.getFallbackImplAttr()
                                 ? module.lookupSymbol<cir::FuncOp>(push.getFallbackImplAttr())
                                 : cir::FuncOp();
        const bool byRvalue = pushFn && takesRvalue(pushFn);
        cir::FuncOp begin = shimFunction(module, "wtc_queue_batch_begin", queuePtr);
        cir::FuncOp deferred = shimFunction(module, "wtc_queue_push_deferred", queuePtr, byRvalue);
        cir::FuncOp end = shimFunction(module, "wtc_queue_batch_end", queuePtr);
        if (!begin || !deferred || !end || begin.getNumArguments() != 2) {
            loop->emitRemark() << "wtc: pushes in this loop not batched: the shim's batch functions "
                                  "for this queue are not in the module";
            return success();
        }
        MLIRContext *ctx = &getContext();
        auto batchPtr = cast<cir::PointerType>(begin.getArgumentTypes()[1]);

        // The batch lives for the whole function, so its slot goes where
        // ClangIR puts every other: the top of the entry block, aligned as
        // the batch type asks (it holds elements, which may be over-aligned).
        auto fn = loop->getParentOfType<cir::FuncOp>();
        OpBuilder top(&fn.getBody().front(), fn.getBody().front().begin());
        const DataLayout dl(module);
        uint64_t align = 8;
        if (auto pointee = batchPtr.getPointee(); pointee && mlir::isa<cir::RecordType>(pointee))
            align = std::max<uint64_t>(align, dl.getTypeABIAlignment(pointee));
        auto slot = cir::AllocaOp::create(top, loop.getLoc(), batchPtr, "wtc.batch",
                                          top.getI64IntegerAttr(static_cast<int64_t>(align)));

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
        // The batch size is the program's (WTC_BATCH_K, 64 unless it sets
        // it): the shim reads it, so the remark names it rather than a number
        // the pass does not know.
        loop->emitRemark() << "wtc: pushes in this loop batched: published every WTC_BATCH_K pushes "
                              "and at the end";
        return success();
    }
};

// The value a mutex operation's handle was cast from.
Value handleSource(Value v) {
    if (auto cast = v.getDefiningOp<UnrealizedConversionCastOp>()) return cast.getInputs().front();
    return v;
}

// What the cost model decided for one loop, in the words its remark ends
// with.
struct CostVerdict {
    bool batch = true;
    std::string words;
    Operation *at = nullptr;  // where an unbounded estimate's reason lies
};

// How many threads take a mutex that is a global, when the module accounts
// for every one: each thread that reaches a lock of it is started once, by
// main, a global constructor or a spawn the module sees, and every lock
// the threads reach names a mutex the walk can place. A lock taken before
// the first thread starts contends with nothing and is not counted.
// Nothing when any of that fails, the walk gives up, or the mutex is not
// a global.
std::optional<unsigned> threadsTaking(ModuleOp module, Value handle) {
    auto gg = handleSource(handle).getDefiningOp<cir::GetGlobalOp>();
    if (!gg) return std::nullopt;
    const StringRef name = gg.getName();
    auto takes = [](Operation *op) { return isa<LockOp, LockBatchBeginOp>(op); };
    ThreadInstanceWalk *self = nullptr;
    auto relevant = [&](cir::FuncOp f) { return self->reachesOp(f, takes); };
    ThreadInstanceWalk walk(module, relevant);
    self = &walk;
    bool unknown = false;
    llvm::SmallVector<std::pair<Operation *, Instance *>, 16> visits;
    {
        // The walk's own remarks belong to the analyses that report on
        // threads; here it only counts.
        ScopedDiagnosticHandler quiet(module.getContext(), [](Diagnostic &d) {
            return success(d.getSeverity() == DiagnosticSeverity::Remark);
        });
        walk.run(
            [&](Instance &inst) {
                inst.func.walk([&](Operation *op) {
                    if (takes(op)) visits.push_back({op, &inst});
                });
            },
            [&](cir::FuncOp) { unknown = true; });
    }
    if (unknown || walk.capExceeded()) return std::nullopt;
    // Resolved once the walk is over, as the queue analysis does: a handle
    // loaded from a field is placed under instances that may not exist
    // until the end.
    llvm::DenseSet<unsigned> threads;
    for (auto [op, inst] : visits) {
        Operation *root = walk.resolveRoot(handleSource(op->getOperand(0)), inst);
        if (!root) return std::nullopt;  // it may be this mutex
        auto g = dyn_cast<cir::GlobalOp>(root);
        if (!g || g.getSymName() != name) continue;
        if (walk.inStartupPhase(op, *inst) && !walk.startupUncertain(*inst)) continue;
        if (inst->threadMult == Mult::Many || inst->speculative || inst->truncated) return std::nullopt;
        threads.insert(inst->thread);
    }
    // None: the loop's own function is not reached from anything the walk
    // starts at, and there is nothing to count.
    if (threads.empty()) return std::nullopt;
    return static_cast<unsigned>(threads.size());
}

// Coarsening pays while the work between two sections is short against
// what handing the mutex over costs, and loses once that work, now done
// with the mutex held, keeps the other threads waiting (decisions/0006).
// Both sides of that are the host's, so the model is a table of classes
// of host, each with the nanoseconds a unit of the estimate takes there
// (what a work round of W8 took, over the 25 units the estimate counts in
// it) and W8's thresholds there (K = 64).
//
// A threshold is on the axis of the estimate, not of W8's work. W8's
// crossover is a level of work, where its speed-up, interpolated linearly
// in the work on the logarithm of the speed-up between the two levels
// around it, crosses 1. But the estimate of W8's loop counts more than that
// work: 212 units with none at all, the loop's own steps and, mostly, the
// two reads of the time-stamp counter, which run on one section in 61 but
// count on every one (a branch counts its larger arm). The threshold is the
// crossover plus that estimate at no work, so that W8's loop is batched on
// that host exactly at the levels of work below its crossover.
struct HostClass {
    const char *name;
    double nsPerUnit;
    double twoThreadsNs;      // two threads
    double fourToEightNs;     // the lower of four and eight threads
};
constexpr HostClass kHosts[] = {
    // Xeon w9-3495X at a fixed 1.9 GHz (benchmarks/results/2026-10-21-eval-w8,
    // timed): a round took 3.71 ns, and W8's loop with no work is estimated at
    // 31 ns; with two threads batching won at 100 ns of work and lost at 200
    // (crossing at 152), with four and eight it won at 200 and lost at 500
    // (245 and 305). 152 + 31 and 245 + 31, rounded down.
    {"sapphire-rapids", 0.148, 180, 275},
    // Xeon Gold 6230 under the powersave governor with turbo
    // (benchmarks/results/2026-10-28-w8-cost, untimed, frequency not fixed):
    // a round took 3.1 ns (median of the leg's runs), and W8's loop with no
    // work is estimated at 26 ns; with two threads batching won at 100 ns of
    // work and lost at 200 (crossing at 177), with four and eight it won at
    // 20 and lost or broke even at 50 (48 and 47). 177 + 26 and 47 + 26,
    // rounded down. Handing the mutex over costs far more here.
    {"cascade-lake", 0.122, 200, 70},
};

class WTCBatchLocksPass : public PassWrapper<WTCBatchLocksPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCBatchLocksPass)

    WTCBatchLocksPass() = default;
    WTCBatchLocksPass(const WTCBatchLocksPass &other) : PassWrapper(other) {}

    StringRef getArgument() const final { return "wtc-batch-locks"; }
    StringRef getDescription() const final {
        return "Hold a mutex across the critical sections of loops in functions marked WTC_BATCH where "
               "nothing in the loop can wait for another thread and the work between sections is short "
               "(run after --lift-cir-to-wtc)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    Option<std::string> cost{
        *this, "cost", llvm::cl::init("estimate"),
        llvm::cl::desc("estimate: batch a legal loop only when the work between its sections is estimated "
                       "under the threshold; off: batch every legal loop")};
    Option<std::string> host{
        *this, "host", llvm::cl::init("sapphire-rapids"),
        llvm::cl::desc("The class of host the program will run on, whose constant and thresholds the "
                       "model takes (decisions/0006 lists them); sapphire-rapids by default, whatever "
                       "the host")};
    Option<double> nsPerUnit{
        *this, "ns-per-unit", llvm::cl::init(0),
        llvm::cl::desc("Nanoseconds a unit of the work estimate takes on the target host (default 0: the "
                       "host class's)")};
    Option<double> thresholdNs{
        *this, "threshold-ns", llvm::cl::init(0),
        llvm::cl::desc("Batch below this many nanoseconds of work between sections, whatever the number "
                       "of threads (default 0: the host class's threshold for the threads the analysis "
                       "counts)")};

    void runOnOperation() override {
        ModuleOp module = getOperation();
        if (cost != "estimate" && cost != "off") {
            module.emitError() << "wtc: --wtc-batch-locks: cost is estimate or off, not '" << cost << "'";
            return signalPassFailure();
        }
        hostClass = nullptr;
        for (const HostClass &h : kHosts)
            if (host == h.name) hostClass = &h;
        if (!hostClass) {
            InFlightDiagnostic d = module.emitError() << "wtc: --wtc-batch-locks: no host class '" << host
                                                      << "'; the classes are";
            for (const HostClass &h : kHosts) d << " " << h.name;
            return signalPassFailure();
        }
        LegalityChecker legal(module, /*strict=*/true);
        WorkEstimator estimator(module);
        llvm::StringMap<std::optional<unsigned>> counted;
        llvm::SmallVector<std::tuple<cir::LoopOpInterface, LockOp, UnlockOp, std::string>, 4> todo;

        module.walk([&](cir::FuncOp f) {
            if (!hasAnnotation(f, "wtc_batch") || f.getBody().empty()) return;
            f.walk([&](cir::LoopOpInterface loop) {
                llvm::SmallVector<Operation *, 2> wtcOps;
                loop->walk([&](Operation *o) {
                    if (isa<WTCDialect>(o->getDialect())) wtcOps.push_back(o);
                });
                // Loops that take no mutex are not this pass's; a loop whose
                // only lock sits in an inner loop is left to that loop.
                LockOp lock;
                for (Operation *o : wtcOps)
                    if (auto l = dyn_cast<LockOp>(o)) { lock = l; break; }
                if (!lock || enclosingLoop(lock) != loop.getOperation()) return;
                UnlockOp unlock;
                if (auto r = check(loop, wtcOps, legal, unlock)) {
                    InFlightDiagnostic d = loop->emitRemark()
                                           << "wtc: critical sections in this loop not batched: " << r->why;
                    if (r->at != loop.getOperation()) d.attachNote(r->at->getLoc()) << "here";
                    return;
                }
                std::string words;
                if (cost == "estimate") {
                    CostVerdict v = decide(module, loop, lock, unlock, estimator, counted);
                    if (!v.batch) {
                        InFlightDiagnostic d = loop->emitRemark()
                                               << "wtc: critical sections in this loop not batched: " << v.words;
                        if (v.at && v.at != loop.getOperation()) d.attachNote(v.at->getLoc()) << "here";
                        return;
                    }
                    words = "; " + v.words;
                }
                todo.push_back({loop, lock, unlock, words});
            });
        });
        for (auto &[loop, lock, unlock, words] : todo)
            if (failed(rewrite(module, loop, lock, unlock, words))) return signalPassFailure();
    }

private:
    const HostClass *hostClass = nullptr;

    static Operation *enclosingLoop(Operation *op) {
        for (Operation *p = op->getParentOp(); p; p = p->getParentOp())
            if (isa<cir::LoopOpInterface>(p)) return p;
        return nullptr;
    }

    // Nanoseconds to a tenth below 10, units whole.
    static std::string ns(double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, v < 10 ? "%.1f" : "%.0f", v);
        return buf;
    }
    static std::string units(double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.0f", v);
        return buf;
    }

    // The work between two sections is one iteration of the loop without
    // its critical section: what runs with the mutex held once the loop is
    // batched and without it before. Below the threshold, or with one
    // thread taking the mutex, the loop is batched.
    CostVerdict decide(ModuleOp module, cir::LoopOpInterface loop, LockOp lock, UnlockOp unlock,
                       WorkEstimator &estimator, llvm::StringMap<std::optional<unsigned>> &counted) {
        Block *block = lock->getBlock();
        auto inSection = [&](Operation *o) {
            return o->getBlock() == block && !o->isBeforeInBlock(lock) && !unlock->isBeforeInBlock(o);
        };
        const WorkEstimate e = estimator.iteration(loop, inSection);
        const HostClass &h = *hostClass;
        const double estNs = e.units * (nsPerUnit > 0 ? nsPerUnit : h.nsPerUnit);
        const std::string estimate =
            e.bounded ? "work between sections estimated at " + ns(estNs) + " ns (" + units(e.units) + " units)"
                      : "work between sections not bounded (" + e.why + ")";

        if (thresholdNs > 0) {
            const std::string t = "the given threshold of " + ns(thresholdNs) + " ns";
            if (e.bounded && estNs <= thresholdNs) return {true, estimate + ", under " + t, nullptr};
            return {false, estimate + ", over " + t, e.at};
        }
        // Under the lowest threshold, the count does not matter; the walk
        // that counts can be costly. A count the analysis cannot give, and
        // one W8 did not measure (three, more than eight), take the lowest.
        const double lowest = std::min(h.twoThreadsNs, h.fourToEightNs);
        const std::string any = "the " + ns(lowest) + " ns threshold that holds for any number of threads";
        if (e.bounded && estNs <= lowest) return {true, estimate + ", under " + any, nullptr};

        std::optional<unsigned> threads;
        if (auto gg = handleSource(lock.getMutex()).getDefiningOp<cir::GetGlobalOp>()) {
            auto [it, fresh] = counted.try_emplace(gg.getName());
            if (fresh) it->second = threadsTaking(module, lock.getMutex());
            threads = it->second;
        }
        if (threads && *threads <= 1)
            return {true, estimate + ", with one thread taking the mutex, which no other waits for", nullptr};
        std::string t;
        double threshold = lowest;
        if (!threads) {
            t = any + " (the threads that take the mutex are not counted here)";
        } else if (*threads == 2 || (*threads >= 4 && *threads <= 8)) {
            threshold = *threads == 2 ? h.twoThreadsNs : h.fourToEightNs;
            t = "the " + ns(threshold) + " ns threshold for the " + std::to_string(*threads) +
                " threads that take the mutex";
        } else {
            t = any + " (" + std::to_string(*threads) + " threads take the mutex, a count not measured)";
        }
        if (e.bounded && estNs <= threshold) return {true, estimate + ", under " + t, nullptr};
        return {false, estimate + ", over " + t, e.at};
    }

    // One mutex: the same handle, or the address of the same global.
    static bool sameMutex(Value a, Value b) {
        a = handleSource(a);
        b = handleSource(b);
        if (a == b) return true;
        auto ga = a.getDefiningOp<cir::GetGlobalOp>(), gb = b.getDefiningOp<cir::GetGlobalOp>();
        return ga && gb && ga.getName() == gb.getName();
    }

    static bool handleInvariant(cir::LoopOpInterface loop, Value v) {
        v = handleSource(v);
        if (!loop->isAncestor(v.getParentBlock()->getParentOp())) return true;
        return static_cast<bool>(v.getDefiningOp<cir::GetGlobalOp>());
    }

    std::optional<Refusal> check(cir::LoopOpInterface loop, llvm::ArrayRef<Operation *> wtcOps,
                                 LegalityChecker &legal, UnlockOp &unlockOut) {
        LockOp lock;
        UnlockOp unlock;
        for (Operation *o : wtcOps) {
            if (auto l = dyn_cast<LockOp>(o); l && !lock) { lock = l; continue; }
            if (auto u = dyn_cast<UnlockOp>(o); u && !unlock) { unlock = u; continue; }
            return Refusal{o, "the loop acts on queues or mutexes other than through one lock and one unlock"};
        }
        if (!unlock) return Refusal{lock, "the loop takes the mutex and does not let it go"};
        if (!sameMutex(lock.getMutex(), unlock.getMutex()))
            return Refusal{unlock, "the loop lets go of a different mutex than it takes"};
        if (!handleInvariant(loop, lock.getMutex()))
            return Refusal{lock, "the loop may change which mutex it takes"};
        // On every iteration, lock first: one block, reached from the body
        // only through scopes.
        Block *block = lock->getBlock();
        if (unlock->getBlock() != block || !lock->isBeforeInBlock(unlock))
            return Refusal{unlock, "the unlock does not follow the lock in the same block"};
        for (Operation *p = lock->getParentOp(); p != loop.getOperation(); p = p->getParentOp())
            if (!isa<cir::ScopeOp, cir::CleanupScopeOp>(p))
                return Refusal{p, "the critical section is not entered on every iteration"};
        std::optional<Refusal> found;
        loop->walk([&](Operation *o) {
            if (isa<cir::ContinueOp>(o)) {
                found = Refusal{o, "a continue skips the critical section on some iterations"};
            } else if (isa<cir::ReturnOp, cir::GotoOp, cir::LabelOp>(o)) {
                found = Refusal{o, "control can leave the loop other than at its end or a break"};
            } else if (isa<cir::BreakOp>(o)) {
                Operation *at = block->findAncestorOpInBlock(*o);
                if (at && lock->isBeforeInBlock(at) && at->isBeforeInBlock(unlock))
                    found = Refusal{o, "a break between the lock and the unlock leaves with the mutex held"};
            } else if (!isa<WTCDialect>(o->getDialect())) {
                found = legal.op(o);
            }
            return found ? WalkResult::interrupt() : WalkResult::advance();
        });
        unlockOut = unlock;
        return found;
    }

    LogicalResult rewrite(ModuleOp module, cir::LoopOpInterface loop, LockOp lock, UnlockOp unlock,
                          StringRef words) {
        auto sourceType = lock.getSourceMutexTypeAttr();
        if (!sourceType) return lock.emitError("wtc: a lock to batch carries no source type");
        Type mutexPtr = sourceType.getValue();
        cir::FuncOp begin = shimFunction(module, "wtc_mutex_batch_begin", mutexPtr);
        cir::FuncOp step = shimFunction(module, "wtc_mutex_batch_step", mutexPtr);
        cir::FuncOp end = shimFunction(module, "wtc_mutex_batch_end", mutexPtr);
        if (!begin || !step || !end || begin.getNumArguments() != 2) {
            loop->emitRemark() << "wtc: critical sections in this loop not batched: the shim's batch "
                                  "functions for this mutex are not in the module";
            return success();
        }
        MLIRContext *ctx = &getContext();
        auto batchPtr = cast<cir::PointerType>(begin.getArgumentTypes()[1]);

        auto fn = loop->getParentOfType<cir::FuncOp>();
        OpBuilder top(&fn.getBody().front(), fn.getBody().front().begin());
        auto slot = cir::AllocaOp::create(top, loop.getLoc(), batchPtr, "wtc.lock_batch",
                                          top.getI64IntegerAttr(4));

        // The handle outside the loop, as for a batched push.
        OpBuilder before(loop.getOperation());
        Value inner = lock.getMutex();
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
        LockBatchBeginOp::create(before, loop.getLoc(), outer, slot.getResult(), ref(begin), sourceType,
                                 ArrayAttr());
        OpBuilder at(unlock);
        LockBatchStepOp::create(at, unlock.getLoc(), unlock.getMutex(), slot.getResult(), ref(step),
                                sourceType, ArrayAttr());
        unlock.erase();
        lock.erase();
        OpBuilder after(loop->getBlock(), std::next(Block::iterator(loop.getOperation())));
        LockBatchEndOp::create(after, loop.getLoc(), outer, slot.getResult(), ref(end), sourceType,
                               ArrayAttr());
        loop->emitRemark() << "wtc: critical sections in this loop batched: the mutex is let go every "
                              "WTC_LOCK_BATCH_K sections and at the end"
                           << words;
        return success();
    }
};

} // namespace

namespace mlir::wtc {
void registerWTCBatchPushesPass() { PassRegistration<WTCBatchPushesPass>(); }
void registerWTCBatchLocksPass() { PassRegistration<WTCBatchLocksPass>(); }
} // namespace mlir::wtc
