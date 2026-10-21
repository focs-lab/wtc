#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/Dominance.h"
#include "mlir/Pass/Pass.h"

#include "llvm/Support/Path.h"

// The substitution. For each queue object the analysis marked spsc, find
// the one call that constructs it and rewrite the constant it passes as the
// topology tag from generic (0) to spsc (1), or to spsc_inline (2) when the
// borrow analysis proved that every pop only borrows its element and the
// element type may be held inline; for one marked mpsc, to mpsc (3), and
// for one marked spmc, to spmc (4). An mpmc queue, or one the analysis
// could not decide, keeps the general-purpose implementation. Nothing else
// in the program changes: the shim's constructor selects the
// implementation from the tag, and every later operation on that object
// follows it.
//
// The pattern, as ClangIR emits it for a defaulted enum argument:
//
//   %q   = cir.get_global @q1 : !cir.ptr<!rec_wtc..queue..>     (or an alloca)
//   %tag = cir.const #cir.int<0> : !s32i
//   cir.call @_ZN3wtc5queueI4ItemEC2ENS_8topologyE(%q, %tag)
//
// Anything that does not match exactly is left alone with a remark: a tag
// that is not a constant, a tag the program already set, no constructor
// call found for the object, or more than one. Leaving the generic
// implementation in place is always correct, so every doubt resolves that
// way.
//
// A side the analysis counts as many only because one counted loop starts
// its threads, one per iteration, has one thread when that loop runs at
// most once, and the program knows whether it will before the loop runs.
// For such a queue the tag becomes a choice made when the queue is built:
//
//   %n    = cir.load %bound                  (the loop's own bound)
//   %one  = cir.cmp le %n, <first + 1>       (at most one iteration)
//   %tag  = cir.select if %one then <spsc> else <mpsc>
//
// with the implementation at each count the one the static verdict would
// give for it. That is sound only if the count read at the constructor is
// the loop's: the constructor runs in the function that runs the loop,
// before it and not in a loop with it, and nothing writes the bound after
// the constructor -- every store to it precedes the constructor in that
// function, and its address goes nowhere else. Otherwise the remark says
// which of these failed, and the static verdict decides as before.
//
// Afterwards every queue operation records, as `impl`, the implementation
// its queue runs, so that later passes need not find the constructor
// again; one chosen at run time records the general-purpose one, which
// every check may assume.

namespace {

class WTCSpecializePass : public PassWrapper<WTCSpecializePass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCSpecializePass)

    WTCSpecializePass() = default;
    WTCSpecializePass(const WTCSpecializePass &other) : PassWrapper(other) {}

    StringRef getArgument() const final { return "wtc-specialize"; }
    StringRef getDescription() const final {
        return "Rewrite the constructor tag of every queue the analysis proved to have one "
               "producer or one consumer (run after --queue-ownership-analysis)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    Option<bool> rewrite{
        *this, "rewrite", llvm::cl::init(true),
        llvm::cl::desc("Rewrite the tags of queues proved spsc, mpsc or spmc. With false the pass only records, "
                       "on each operation, the implementation its queue runs as the program "
                       "stands: the build of an unmodified source.")};

    ListOption<std::string> forceInline{
        *this, "force-inline",
        llvm::cl::desc("Global queue symbols to tag spsc_inline regardless of the verdicts. "
                       "For the negative control only: a consumer that keeps what it pops then "
                       "reads an element that has been destroyed.")};

    ListOption<std::string> forceSpsc{
        *this, "force-spsc",
        llvm::cl::desc("Global queue symbols to tag spsc regardless of the verdict. "
                       "For the negative control only; this is how to build a program that races.")};

    void runOnOperation() override {
        ModuleOp module = getOperation();
        MLIRContext *ctx = &getContext();

        // Every constructor call, keyed by the root object it constructs. A
        // queue that is a member of a struct or a heap object has the one
        // call that builds it as its root.
        auto ctorsOf = collectQueueConstructors(module);
        module.walk([&](cir::CallOp call) {
            if (call->hasAttr(kTopologyAttrName)) ctorsOf[call.getOperation()] = {call};
        });

        unsigned rewritten = 0;
        module.walk([&](Operation *root) {
            if (!rewrite) return;
            if (!isa<cir::GlobalOp, cir::AllocaOp>(root) &&
                !(isa<cir::CallOp>(root) && root->hasAttr(kTopologyAttrName)))
                return;
            bool wanted = false, forcedInline = false, forced = false;
            Topology verdict = Topology::unknown;
            if (auto t = root->getAttrOfType<TopologyAttr>(kTopologyAttrName)) {
                verdict = t.getValue();
                wanted = verdict == Topology::spsc || verdict == Topology::mpsc ||
                         verdict == Topology::spmc;
            }
            if (auto g = dyn_cast<cir::GlobalOp>(root)) {
                for (const std::string &s : forceSpsc)
                    if (g.getSymName() == s) wanted = forced = true;
                for (const std::string &s : forceInline)
                    if (g.getSymName() == s) wanted = forcedInline = true;
            }
            if (!forced && !forcedInline && verdict != Topology::unknown &&
                specialiseAtRunTime(root, verdict, ctorsOf, module)) {
                ++rewritten;
                return;
            }
            if (!wanted) return;

            auto it = ctorsOf.find(root);
            if (it == ctorsOf.end() || it->second.empty()) {
                root->emitRemark() << "wtc: not specialised: no constructor call found for this queue";
                return;
            }
            if (it->second.size() != 1) {
                root->emitRemark() << "wtc: not specialised: " << it->second.size()
                                   << " constructor calls reach this queue";
                return;
            }
            cir::CallOp ctor = it->second.front();
            if (ctor.getNumOperands() != 2) {
                ctor.emitRemark() << "wtc: not specialised: the constructor does not take a topology tag";
                return;
            }
            auto tagConst = ctor.getOperand(1).getDefiningOp<cir::ConstantOp>();
            auto tagVal = tagConst ? dyn_cast<cir::IntAttr>(tagConst.getValue()) : cir::IntAttr();
            if (!tagVal) {
                ctor.emitRemark() << "wtc: not specialised: the topology tag is not a constant";
                return;
            }
            if (tagVal.getSInt() != 0) {
                ctor.emitRemark() << "wtc: not specialised: the program sets the tag itself ("
                                  << tagVal.getSInt() << "); left as declared";
                return;
            }
            // Inline storage needs both the borrow proof and an element type
            // the shim can hold inline, which its constructor says by calling
            // a marker; without either, plain spsc. Many producers and one
            // consumer is the list without locks, one producer and many
            // consumers the ring whose consumers take turns, whatever the
            // pops do.
            const bool manySide = (verdict == Topology::mpsc || verdict == Topology::spmc) &&
                                  !forced && !forcedInline;
            Impl to = !manySide ? Impl::spsc : verdict == Topology::mpsc ? Impl::mpsc : Impl::spmc;
            if (forcedInline) {
                to = Impl::spsc_inline;
            } else if (!manySide && root->hasAttr(kBorrowedAttrName)) {
                if (marksInlineOk(lookupCallee(ctor)))
                    to = Impl::spsc_inline;
                else
                    ctor.emitRemark() << "wtc: every pop borrows, but this element type cannot be "
                                         "held inline; plain spsc";
            }
            // A fresh constant rather than a mutation of the existing one: at
            // -O0 each use has its own, but nothing guarantees that.
            OpBuilder b(ctor);
            auto tag = cir::ConstantOp::create(
                b, ctor.getLoc(), tagConst.getType(),
                cir::IntAttr::get(tagConst.getType(), static_cast<int64_t>(to)));
            ctor->setOperand(1, tag.getResult());
            root->setAttr(kSpecializedAttrName, TopologyAttr::get(ctx, manySide ? verdict : Topology::spsc));
            ctor.emitRemark() << "wtc: specialised: constructor tag rewritten to "
                              << stringifyImpl(to);
            ++rewritten;
        });
        (void)rewritten;
        recordImplementations(module, ctorsOf);
    }

    using CtorMap = llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>>;

    // The implementation for one producer or many and one consumer or many.
    static Impl implAt(bool oneProducer, bool oneConsumer, bool inlineOk) {
        if (oneProducer && oneConsumer) return inlineOk ? Impl::spsc_inline : Impl::spsc;
        if (oneConsumer) return Impl::mpsc;
        if (oneProducer) return Impl::spmc;
        return Impl::generic;
    }

    // Whether `before` runs before `after` in their function, whatever path
    // is taken: their ancestors in one block are in that order and no loop
    // encloses that block.
    enum class Order { Before, NotBefore, InLoop };
    static Order order(Operation *before, Operation *after, cir::FuncOp f) {
        Operation *x = after;
        while (x && x != f.getOperation() && !x->getBlock()->findAncestorOpInBlock(*before))
            x = x->getParentOp();
        if (!x || x == f.getOperation()) return Order::NotBefore;
        Operation *y = x->getBlock()->findAncestorOpInBlock(*before);
        if (y == x || !y->isBeforeInBlock(x)) return Order::NotBefore;
        for (Operation *p = x->getParentOp(); p && p != f.getOperation(); p = p->getParentOp())
            if (isa<cir::LoopOpInterface>(p)) return Order::InLoop;
        return Order::Before;
    }

    // Why the bound the loop reads might not be, at the constructor, the
    // value the loop will read; null when it must be.
    static const char *boundUnsettled(cir::CallOp ctor, CountedLoop l, ModuleOp module) {
        auto f = ctor->getParentOfType<cir::FuncOp>();
        if (l.loop->getParentOfType<cir::FuncOp>() != f)
            return "the queue is built outside the function whose loop starts its threads";
        switch (order(ctor, l.loop, f)) {
        case Order::Before: break;
        case Order::NotBefore: return "the queue is not built before the loop that starts its threads";
        case Order::InLoop: return "the queue is built in a loop around the loop that starts its threads";
        }
        auto storesSettled = [&](Value addr) -> const char * {
            for (Operation *u : addr.getUsers()) {
                if (auto ld = dyn_cast<cir::LoadOp>(u); ld && ld.getAddr() == addr) continue;
                if (isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
                auto st = dyn_cast<cir::StoreOp>(u);
                if (!st || st.getAddr() != addr) return "the loop's bound has its address taken";
                if (st->getParentOfType<cir::FuncOp>() != f)
                    return "the loop's bound is written in another function";
                if (order(st, ctor, f) != Order::Before)
                    return "the loop's bound may be written after the queue is built";
            }
            return nullptr;
        };
        Value addr = l.bound.getAddr();
        if (auto slot = addr.getDefiningOp<cir::AllocaOp>()) {
            DominanceInfo dom(f);
            if (!dom.properlyDominates(slot.getOperation(), ctor.getOperation()))
                return "the loop's bound is a variable the queue's constructor cannot see";
            return storesSettled(addr);
        }
        auto gg = addr.getDefiningOp<cir::GetGlobalOp>();
        if (!gg) return "the loop's bound is neither a variable nor a global";
        const char *why = nullptr;
        module.walk([&](cir::GetGlobalOp use) {
            if (!why && use.getName() == gg.getName()) why = storesSettled(use.getResult());
        });
        return why;
    }

    // `bound <= first + 1` (`bound <= first` for an inclusive loop), read
    // just before the constructor: whether the loop will run at most once.
    static Value atMostOnce(OpBuilder &b, Location loc, CountedLoop l) {
        Value addr = l.bound.getAddr();
        if (auto gg = addr.getDefiningOp<cir::GetGlobalOp>()) addr = b.clone(*gg)->getResult(0);
        auto n = cir::LoadOp::create(b, loc, addr);
        auto lim = cir::ConstantOp::create(
            b, loc, n.getType(), cir::IntAttr::get(n.getType(), l.first + (l.inclusive ? 0 : 1)));
        return cir::CmpOp::create(b, loc, cir::CmpOpKind::le, n, lim);
    }

    static cir::ForOp loopNumbered(ModuleOp module, IntegerAttr id) {
        cir::ForOp found;
        if (!id) return found;
        module.walk([&](cir::ForOp f) {
            if (auto a = f->getAttrOfType<IntegerAttr>(kSpawnLoopAttrName); a && a.getInt() == id.getInt())
                found = f;
        });
        return found;
    }

    // The tag chosen when the queue is built, for a queue with a side whose
    // threads one counted loop starts. True when the constructor now makes
    // that choice; false leaves the queue to the static verdict, with a
    // remark when the reason is the loop.
    bool specialiseAtRunTime(Operation *root, Topology verdict, CtorMap &ctorsOf, ModuleOp module) {
        auto pId = root->getAttrOfType<IntegerAttr>(kProducersLoopAttrName);
        auto cId = root->getAttrOfType<IntegerAttr>(kConsumersLoopAttrName);
        if (!pId && !cId) return false;
        auto it = ctorsOf.find(root);
        if (it == ctorsOf.end() || it->second.size() != 1) return false;
        cir::CallOp ctor = it->second.front();
        if (ctor.getNumOperands() != 2) return false;
        auto tagConst = ctor.getOperand(1).getDefiningOp<cir::ConstantOp>();
        auto tagVal = tagConst ? dyn_cast<cir::IntAttr>(tagConst.getValue()) : cir::IntAttr();
        if (!tagVal || tagVal.getSInt() != 0) return false;

        std::optional<CountedLoop> pl, cl;
        for (auto [id, out] : {std::pair{pId, &pl}, std::pair{cId, &cl}}) {
            if (!id) continue;
            cir::ForOp loop = loopNumbered(module, id);
            if (!loop || !(*out = countedLoop(loop))) return false;
            if (const char *why = boundUnsettled(ctor, **out, module)) {
                ctor.emitRemark() << "wtc: not specialised at run time: " << why;
                return false;
            }
        }
        const bool manyP = verdict == Topology::mpsc || verdict == Topology::mpmc;
        const bool manyC = verdict == Topology::spmc || verdict == Topology::mpmc;
        const bool inlineOk = root->hasAttr(kBorrowedAttrName) && marksInlineOk(lookupCallee(ctor));

        OpBuilder b(ctor);
        const Location loc = ctor.getLoc();
        Value oneP = pl ? atMostOnce(b, loc, *pl) : Value();
        Value oneC = cl ? atMostOnce(b, loc, *cl) : Value();
        auto k = [&](Impl i) -> Value {
            return cir::ConstantOp::create(b, loc, tagConst.getType(),
                                           cir::IntAttr::get(tagConst.getType(), static_cast<int64_t>(i)));
        };
        // Each select's operands are made in statements of their own: the
        // order in which a call's arguments are evaluated is unspecified,
        // and it is the order in which the constants appear in the IR.
        auto forProducers = [&](bool oneProducer) -> Value {
            if (!oneC) return k(implAt(oneProducer, !manyC, inlineOk));
            Value one = k(implAt(oneProducer, true, inlineOk));
            Value many = k(implAt(oneProducer, false, inlineOk));
            return cir::SelectOp::create(b, loc, oneC, one, many).getResult();
        };
        Value tag;
        if (oneP) {
            Value one = forProducers(true);
            Value many = forProducers(false);
            tag = cir::SelectOp::create(b, loc, oneP, one, many).getResult();
        } else {
            tag = forProducers(!manyP);
        }
        ctor->setOperand(1, tag);
        root->setAttr(kSpecializedAttrName, TopologyAttr::get(&getContext(), verdict));

        // What each count gives, in the remark.
        auto where = [](const std::optional<CountedLoop> &l) { return whereOf(l->loop->getLoc()); };
        auto at = [&](bool oneProducer, bool oneConsumer) {
            return stringifyImpl(implAt(oneProducer, oneConsumer, inlineOk));
        };
        InFlightDiagnostic d = ctor.emitRemark() << "wtc: specialised at run time: ";
        if (pl && cl) {
            d << at(true, true) << " when the loops at " << where(pl) << " (producers) and " << where(cl)
              << " (consumers) each run at most once, " << at(false, true) << " when only the second does, "
              << at(true, false) << " when only the first does, else " << at(false, false);
        } else {
            const bool other = pl ? !manyC : !manyP;
            const Impl once = pl ? implAt(true, other, inlineOk) : implAt(other, true, inlineOk);
            const Impl more = pl ? implAt(false, other, inlineOk) : implAt(other, false, inlineOk);
            d << stringifyImpl(once) << " when the loop at " << where(pl ? pl : cl) << " ("
              << (pl ? "producers" : "consumers") << ") runs at most once, else " << stringifyImpl(more);
        }
        return true;
    }

    static std::string whereOf(Location loc) {
        if (auto f = dyn_cast<FileLineColLoc>(loc))
            return (llvm::sys::path::filename(f.getFilename().getValue()) + ":" + Twine(f.getLine()) + ":" +
                    Twine(f.getColumn()))
                .str();
        if (auto f = dyn_cast<FusedLoc>(loc))
            for (Location inner : f.getLocations())
                if (std::string w = whereOf(inner); !w.empty()) return w;
        if (auto n = dyn_cast<NameLoc>(loc)) return whereOf(n.getChildLoc());
        return "";
    }

    // The shim's constructor calls a function annotated wtc_queue_inline_ok
    // exactly when its element type may be held inline.
    static bool marksInlineOk(cir::FuncOp ctor) {
        if (!ctor || ctor.getBody().empty()) return false;
        bool found = false;
        ctor.walk([&](cir::CallOp call) {
            if (hasAnnotation(lookupCallee(call), "wtc_queue_inline_ok")) found = true;
        });
        return found;
    }

    // What each numbered queue runs, read back from the tag its one
    // constructor passes once the rewriting above is done, and written on
    // every operation that can only act on that queue. Anything else -- no
    // constructor found, several, a tag that is not a constant, an
    // operation that may act on several queues -- is the general-purpose
    // implementation: the answer that is never wrong to assume.
    static void recordImplementations(
        ModuleOp module, const llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>> &ctorsOf) {
        MLIRContext *ctx = module.getContext();
        llvm::DenseMap<int64_t, Impl> implOf;
        for (auto &[root, ctors] : ctorsOf) {
            auto id = root->getAttrOfType<IntegerAttr>(kQueueIdAttrName);
            if (!id) continue;
            Impl impl = Impl::generic;
            if (auto tag = constructorTag(ctors); tag && *tag >= 0)
                if (auto known = symbolizeImpl(static_cast<uint32_t>(*tag))) impl = *known;
            implOf[id.getInt()] = impl;
        }
        auto implFor = [&](IntegerAttr id) {
            if (!id) return Impl::generic;
            auto it = implOf.find(id.getInt());
            return it == implOf.end() ? Impl::generic : it->second;
        };
        module.walk([&](Operation *op) {
            if (!isa<QueuePushOp, QueuePopOp, QueueBatchBeginOp, QueuePushDeferredOp, QueueBatchEndOp>(op))
                return;
            op->setAttr("impl", ImplAttr::get(ctx, implFor(op->getAttrOfType<IntegerAttr>("queue_id"))));
        });
    }
};

} // namespace

namespace mlir::wtc {
    void registerWTCSpecializePass() {
        PassRegistration<WTCSpecializePass>();
    }
}
