#include "WTC/ProgressTable.h"
#include "WTC/Transforms/Blocking.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/ProgressRequirements.h"
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
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"
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
// Either end may be one call away from that function. The loop may be in
// a function called from it, its bound a parameter that function never
// writes, passed a variable at the call the analysis recorded (ferret's
// tpool_create(desc, NTHREAD_SEG)): the bound is then that variable, read
// at the call, and the rules above apply to the call in place of the
// loop. The constructor may be in a function called from it, once, before
// the loop (spdlog's pool builds its queue object, which builds the lifted
// queue): that function is copied with the tag as one more parameter, the
// constructor in the copy passes it, and the one call is redirected to the
// copy with the tag chosen just before it. Other callers keep the original
// and its static tag.
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
            if (!takesTopologyTag(ctor)) {
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
            // consumers the queue whose consumers claim cells, whatever the
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
        if (rewrite) meetRequirements(module, ctorsOf);
        recordImplementations(module, ctorsOf);
    }

    // The implementations the runtime the program is built with announces,
    // beyond the five every runtime has: its queue's constructor calls a
    // marker annotated wtc_queue_impl_<name> with the tag that selects it,
    // a constant (the runtime's numbering, not the compiler's), and for an
    // implementation built for a number of threads that number as a second
    // constant (shim/bounded.hpp: the rings of indices). Read from the
    // constructor and the functions it calls directly. A runtime that has
    // no such marker offers nothing more, and nothing more is chosen.
    struct Offer {
        Impl impl;
        int64_t tag;
        // The most threads it may serve: operations in flight at once, and
        // for wcq threads alive at once that have operated on it. None
        // when the marker states no bound.
        std::optional<uint64_t> threads;
    };
    static llvm::SmallVector<Offer, 6> runtimeOffers(cir::FuncOp ctor) {
        llvm::SmallVector<Offer, 6> out;
        if (!ctor || ctor.getBody().empty()) return out;
        // A constant, or a load of a constant global with an integer
        // initialiser (a static constexpr member passed by value, as the
        // shim's kIndexThreads is).
        auto constant = [](Value v) -> std::optional<int64_t> {
            if (auto ld = v.getDefiningOp<cir::LoadOp>()) {
                auto gg = ld.getAddr().getDefiningOp<cir::GetGlobalOp>();
                auto g = gg ? SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr())
                            : cir::GlobalOp();
                if (!g || !g.getConstant() || !g.getInitialValue()) return std::nullopt;
                if (auto i = dyn_cast<cir::IntAttr>(*g.getInitialValue())) return i.getSInt();
                return std::nullopt;
            }
            auto k = v.getDefiningOp<cir::ConstantOp>();
            auto i = k ? dyn_cast<cir::IntAttr>(k.getValue()) : cir::IntAttr();
            if (!i) return std::nullopt;
            return i.getSInt();
        };
        auto scan = [&](cir::FuncOp f) {
            f.walk([&](cir::CallOp call) {
                cir::FuncOp callee = lookupCallee(call);
                auto annotations = callee ? callee.getAnnotationsAttr() : ArrayAttr();
                if (!annotations || call.getNumOperands() < 1) return;
                for (Attribute a : annotations) {
                    auto ann = dyn_cast<cir::AnnotationAttr>(a);
                    llvm::StringRef name = ann ? ann.getName().getValue() : llvm::StringRef();
                    if (!name.consume_front("wtc_queue_impl_")) continue;
                    auto impl = symbolizeImpl(name);
                    auto tag = constant(call.getOperand(0));
                    if (!impl || !tag || static_cast<uint32_t>(*impl) <= static_cast<uint32_t>(Impl::spmc)) continue;
                    Offer o{*impl, *tag, std::nullopt};
                    // A bound that is not a constant, or not positive, is no
                    // bound the compiler can meet: nothing is offered.
                    if (call.getNumOperands() >= 2) {
                        auto n = constant(call.getOperand(1));
                        if (!n || *n <= 0) continue;
                        o.threads = static_cast<uint64_t>(*n);
                    }
                    if (llvm::none_of(out, [&](const Offer &x) { return x.impl == o.impl && x.tag == o.tag; }))
                        out.push_back(o);
                }
            });
        };
        scan(ctor);
        ctor.walk([&](cir::CallOp call) {
            if (cir::FuncOp callee = lookupCallee(call); callee && !callee.getBody().empty()) scan(callee);
        });
        return out;
    }

    // The implementation a constructor's tag selects: one of the five by
    // its number, one the runtime announces by the tag it announced, and
    // otherwise the general-purpose one.
    static Impl implOfTag(std::optional<int64_t> tag, cir::CallOp ctor) {
        if (!tag || *tag < 0) return Impl::generic;
        for (const Offer &o : runtimeOffers(lookupCallee(ctor)))
            if (o.tag == *tag) return o.impl;
        if (*tag <= static_cast<int64_t>(Impl::spmc))
            if (auto known = symbolizeImpl(static_cast<uint32_t>(*tag))) return *known;
        return Impl::generic;
    }

    static std::optional<OpForm> formOf(Operation *op) {
        if (auto p = dyn_cast<QueuePushOp>(op)) return p.getForm();
        if (auto p = dyn_cast<QueuePopOp>(op)) return p.getForm();
        if (isa<QueueBatchBeginOp>(op)) return OpForm::batch_begin;
        if (isa<QueuePushDeferredOp>(op)) return OpForm::push_deferred;
        if (isa<QueueBatchEndOp>(op)) return OpForm::batch_end;
        return std::nullopt;
    }

    // Why an operation's row does not meet a requirement, in words after
    // "its <form>", or nothing: the criteria the progress check applies to
    // the operation alone (WTCCheckProgress.cpp), which a path through it
    // cannot do better than. Every requirement refuses a wait for data
    // (a real-time thread's apart, whose waits the check holds to parking),
    // the allocator and a first-use effect; a real-time thread and a
    // declared wait-free one need wait-freedom; within k a worst case of at
    // most k steps under the counts proved; progress-critical a worst case
    // that is a polynomial in the counts.
    static std::optional<std::string> unmet(const ProgressSpec &s, const RequirementRoot &r,
                                            const ThreadCounts &counts, OpForm form, Impl impl) {
        const bool realtime = r.source == RequirementRoot::Source::realtime;
        // A handler's push may interrupt a push of its own thread that holds
        // a cell; only a consumer that looks past such a cell gets past it
        // (WTCCheckProgress.cpp, the handler's push).
        const bool pushes = form == OpForm::push || form == OpForm::push_deferred || form == OpForm::batch_end ||
                            form == OpForm::bounded_try_push || form == OpForm::bounded_push_wait ||
                            form == OpForm::bounded_push_overrun || form == OpForm::bounded_push_batch;
        if (r.source == RequirementRoot::Source::handler && pushes) {
            const bool looksPast = isBoundedForm(form) ? impl == Impl::scq || impl == Impl::wcq
                                                       : impl == Impl::mpsc || impl == Impl::jiffy;
            if (!looksPast) return std::string("leaves the consumer waiting for a push the handler may interrupt");
        }
        const Level need = realtime || r.req.kind >= Requirement::Kind::wait_free ? Level::wait_free : Level::lock_free;
        if (s.partial && !realtime) return std::string("waits for data");
        if (s.level < need) return "is " + describeLevel(s.level).str();
        if (s.allocates()) return std::string("may call the allocator");
        if (s.firstUse) return "takes, on first use, a " + describeFirstUse(s.firstUse);
        if (r.req.within) {
            auto v = s.bound.instantiate(counts);
            if (!v) return "has a worst case that is not a number here: " + s.bound.str();
            if (*v > *r.req.within) return "may take " + std::to_string(*v) + " steps";
        }
        if (r.req.kind == Requirement::Kind::critical && !s.bound.isPoly())
            return "has a worst case " + s.bound.str();
        return std::nullopt;
    }

    // Why an implementation built for a number of threads may not serve
    // this queue, or none: the producers and consumers proved, summed, must
    // be a number within the marker's (a thread on both sides counts
    // twice, which overstates), and no operation of the queue may run the
    // element's own code, since an operation that code starts on the same
    // queue is one more in flight (shim/bounded.hpp).
    static std::optional<std::string> beyondBound(const Offer &o, const ThreadCounts &counts,
                                                  llvm::ArrayRef<Operation *> ops, ElementMembers &elements) {
        const Count total = counts.producers + counts.consumers;
        const auto n = total.value();
        if (!n || *n > *o.threads)
            return "needs at most " + std::to_string(*o.threads) +
                   " threads operating on the queue, and the producers and consumers proved are " + total.str();
        for (Operation *op : ops) {
            ElementMembers::Found found =
                elements.of(op, element_copy_ctor | element_move_ctor | element_move_assign | element_dtor);
            if (found.unknown) return std::string("counts operations in flight, and the element type is not known here");
            if (!found.funcs.empty())
                return "counts operations in flight, and the element's own " +
                       llvm::demangle(found.funcs.front().getSymName().str()) +
                       " runs inside each, which may start another on the same queue";
        }
        return std::nullopt;
    }

    // A queue whose operations sit on a path with a requirement
    // (ProgressRequirements.h) gets the cheapest implementation that meets
    // it under the counts proved, when the one chosen above does not: for
    // one consumer and no capacity jiffy, wait-free; for a capacity the
    // settled ring of the queue's topology, whose single side does not wait
    // for the other and which needs no bound on threads, then scq,
    // lock-free, then wcq, wait-free (docs/progress.md, "Choosing for a
    // requirement"). A ring of indices is chosen only when the producers
    // and consumers proved are within the threads its marker states and
    // the element runs no code of its own inside an operation, which could
    // start another on the same queue and which the bound counts apart.
    // Only an implementation the runtime announces is chosen; otherwise a
    // remark says what is missing. A real-time thread's wait for data
    // counts by the level of the step that takes the element.
    void meetRequirements(ModuleOp module, llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>> &ctorsOf) {
        RequirementRoots requirements(module);
        if (requirements.roots().empty()) return;
        ElementMembers elements(module);
        llvm::DenseMap<int64_t, llvm::SmallVector<Operation *, 4>> opsOf;
        module.walk([&](Operation *op) {
            if (!formOf(op)) return;
            if (auto id = op->getAttrOfType<IntegerAttr>("queue_id")) opsOf[id.getInt()].push_back(op);
        });
        MLIRContext *ctx = module.getContext();
        for (auto &[root, ctors] : ctorsOf) {
            auto id = root->getAttrOfType<IntegerAttr>(kQueueIdAttrName);
            auto topo = root->getAttrOfType<TopologyAttr>(kTopologyAttrName);
            if (!id || !topo || topo.getValue() == Topology::unknown) continue;
            // Every operation of the queue on a path with a requirement, with
            // every requirement that reaches it: an implementation is chosen
            // only if each of them still holds for each of these
            // operations, co-located ones included (jiffy's pop is not
            // bounded in the counts where the mpsc pop is).
            struct Need {
                Operation *op;
                llvm::SmallVector<const RequirementRoot *, 2> by;
            };
            llvm::SmallVector<Need, 4> needs;
            bool bounded = false, keepsRing = false;
            for (Operation *op : opsOf.lookup(id.getInt())) {
                const OpForm form = *formOf(op);
                bounded |= isBoundedForm(form);
                keepsRing |= form == OpForm::bounded_push_overrun || form == OpForm::bounded_push_batch ||
                             form == OpForm::bounded_pop_batch;
                auto by = requirements.allReaching(op->getParentOfType<cir::FuncOp>());
                if (!by.empty()) needs.push_back({op, {by.begin(), by.end()}});
            }
            if (needs.empty()) continue;
            if (ctors.size() != 1) {
                root->emitRemark() << "wtc: not specialised for a requirement: the queue is built by "
                                   << ctors.size() << " constructor calls, and a tag is chosen for one";
                continue;
            }
            ThreadCounts counts;
            if (auto p = root->getAttrOfType<StringAttr>(kProducersCountAttrName)) counts.producers = Count::parse(p);
            if (auto c = root->getAttrOfType<StringAttr>(kConsumersCountAttrName)) counts.consumers = Count::parse(c);
            // The first operation and requirement an implementation fails,
            // and why, in words.
            struct Miss {
                const Need *need = nullptr;
                const RequirementRoot *by = nullptr;
                std::string why;
            };
            auto missOf = [&](Impl impl) -> std::optional<Miss> {
                for (const Need &n : needs) {
                    const ProgressSpec spec = progressSpecFor(*formOf(n.op), impl, counts);
                    for (const RequirementRoot *r : n.by)
                        if (auto why = unmet(spec, *r, counts, *formOf(n.op), impl))
                            return Miss{&n, r, "its " + stringifyOpForm(*formOf(n.op)).str() + " " + *why};
                }
                return std::nullopt;
            };
            cir::CallOp ctor = ctors.front();
            const Impl current = implOfTag(constructorTag(ctors), ctor);
            auto currentMiss = missOf(current);
            if (!currentMiss) continue;
            const Miss first = *currentMiss;
            // A tag the program states itself is left alone, as the
            // topology's is.
            auto tagConst = takesTopologyTag(ctor) ? ctor.getOperand(1).getDefiningOp<cir::ConstantOp>()
                                                   : cir::ConstantOp();
            auto tagVal = tagConst ? dyn_cast<cir::IntAttr>(tagConst.getValue()) : cir::IntAttr();
            if (!tagVal || (tagVal.getSInt() != 0 && !root->hasAttr(kSpecializedAttrName))) {
                ctor.emitRemark() << "wtc: not specialised for a requirement: the topology tag is "
                                  << (tagVal ? "the program's own" : "not a constant") << "; "
                                  << llvm::demangle(cir::FuncOp(first.by->func).getSymName().str()) << ", "
                                  << first.by->why << ": " << stringifyImpl(current) << ": " << first.why;
                continue;
            }
            const bool oneConsumer = topo.getValue() == Topology::spsc || topo.getValue() == Topology::mpsc;
            llvm::SmallVector<Impl, 3> candidates;
            if (bounded && !keepsRing) {
                switch (topo.getValue()) {
                case Topology::spsc: candidates.push_back(Impl::spsc_settled); break;
                case Topology::mpsc: candidates.push_back(Impl::mpsc_settled); break;
                case Topology::spmc: candidates.push_back(Impl::spmc_settled); break;
                default: break;
                }
                candidates.append({Impl::scq, Impl::wcq});
            } else if (!bounded && oneConsumer) {
                candidates = {Impl::jiffy};
            }
            auto offers = runtimeOffers(lookupCallee(ctor));
            llvm::SmallVector<std::string, 4> refused;
            bool done = false;
            for (Impl c : candidates) {
                if (c == current) continue;
                const std::string name = stringifyImpl(c).str();
                if (auto m = missOf(c)) {
                    refused.push_back(name + ": " + m->why);
                    continue;
                }
                auto it = llvm::find_if(offers, [&](const Offer &o) { return o.impl == c; });
                if (it == offers.end()) {
                    refused.push_back(name + ": the runtime here does not offer it");
                    continue;
                }
                if (it->threads) {
                    if (auto why = beyondBound(*it, counts, opsOf.lookup(id.getInt()), elements)) {
                        refused.push_back(name + ": it " + *why);
                        continue;
                    }
                }
                OpBuilder b(ctor);
                auto tag = cir::ConstantOp::create(b, ctor.getLoc(), tagConst.getType(),
                                                   cir::IntAttr::get(tagConst.getType(), it->tag));
                ctor->setOperand(1, tag.getResult());
                root->setAttr(kSpecializedAttrName, TopologyAttr::get(ctx, topo.getValue()));
                ctor.emitRemark() << "wtc: specialised: constructor tag rewritten to " << name << ", for "
                                  << llvm::demangle(cir::FuncOp(first.by->func).getSymName().str()) << ", "
                                  << first.by->why;
                done = true;
                break;
            }
            if (done) continue;
            InFlightDiagnostic d = ctor.emitRemark()
                                   << "wtc: no implementation meets a requirement: "
                                   << llvm::demangle(cir::FuncOp(first.by->func).getSymName().str()) << ", "
                                   << first.by->why << ": " << stringifyImpl(current) << ": " << first.why;
            if (candidates.empty() && bounded)
                d << "; a queue built to overrun or to take batches keeps its ring whatever the tag";
            else if (candidates.empty())
                d << "; an unbounded queue with many consumers has no other implementation here, and a capacity "
                     "would allow a ring of indices";
            for (const std::string &r : refused) d << "; " << r;
            d.attachNote(first.need->op->getLoc()) << "the operation";
        }
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

    // Where a side's count is read in the function that builds the queue:
    // the loop's bound, at the loop; or, for a loop one call down (`via`),
    // the variable the call passes for the bound, at the call.
    struct SideBound {
        CountedLoop l;
        Value addr;       // the variable read: a slot or a global
        Operation *at;    // the loop, or the call that runs it
        Operation *via = nullptr;
    };

    // The bound of a loop reached through `via`, a call of the loop's
    // function: a parameter of that function, kept in its slot and never
    // written there again, its address going nowhere, and passed at the
    // call as a variable's value, as it is, of the parameter's own type.
    static const char *boundThrough(CountedLoop l, Operation *via, SideBound &out) {
        auto call = dyn_cast<cir::CallOp>(via);
        auto loopFn = l.loop->getParentOfType<cir::FuncOp>();
        if (!call || lookupCallee(call) != loopFn) return "the call recorded for the loop does not call its function";
        auto slot = l.bound.getAddr().getDefiningOp<cir::AllocaOp>();
        if (!slot) return "the loop's bound is not a parameter of its function";
        BlockArgument param;
        for (Operation *u : slot->getUsers()) {
            if (auto ld = dyn_cast<cir::LoadOp>(u); ld && ld.getAddr() == slot.getResult()) continue;
            if (isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
            auto st = dyn_cast<cir::StoreOp>(u);
            auto arg = st ? dyn_cast<BlockArgument>(st.getValue()) : BlockArgument();
            if (!st || st.getAddr() != slot.getResult() || !arg || param ||
                arg.getOwner() != &loopFn.getBody().front() || st->getBlock() != &loopFn.getBody().front())
                return "the loop's bound is not a parameter its function keeps as it is";
            param = arg;
        }
        if (!param) return "the loop's bound is not a parameter of its function";
        if (param.getArgNumber() >= call.getNumOperands()) return "the call passes no value for the loop's bound";
        auto ld = call.getOperand(param.getArgNumber()).getDefiningOp<cir::LoadOp>();
        if (!ld || ld.getType() != param.getType())
            return "the call passes the loop's bound as something other than a variable";
        out.l = l;
        out.addr = ld.getAddr();
        out.at = via;
        out.via = via;
        return nullptr;
    }

    // Why the bound the loop reads might not be, at the constructor (or
    // the call that builds the queue, `ctor`), the value the loop will
    // read; null when it must be.
    static const char *boundUnsettled(Operation *ctor, const SideBound &b, ModuleOp module) {
        auto f = ctor->getParentOfType<cir::FuncOp>();
        if (b.at->getParentOfType<cir::FuncOp>() != f)
            return "the queue is built outside the function whose loop starts its threads";
        switch (order(ctor, b.at, f)) {
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
        Value addr = b.addr;
        if (auto slot = addr.getDefiningOp<cir::AllocaOp>()) {
            DominanceInfo dom(f);
            if (!dom.properlyDominates(slot.getOperation(), ctor))
                return "the loop's bound is a variable the queue's constructor cannot see";
            return storesSettled(addr);
        }
        auto gg = addr.getDefiningOp<cir::GetGlobalOp>();
        if (!gg) return "the loop's bound is neither a variable nor a global";
        // A global's address also travels as a global_view attribute: in a
        // pointer's initialiser, in a constant record. A store through such a
        // pointer is a store the walk over get_global never sees, so any
        // global_view that names the bound makes it unsettled.
        bool viewed = false;
        module.walk([&](Operation *op) {
            if (viewed) return;
            for (NamedAttribute a : op->getAttrs())
                a.getValue().walk([&](cir::GlobalViewAttr gv) {
                    if (gv.getSymbol().getValue() == gg.getName()) viewed = true;
                });
        });
        if (viewed) return "the loop's bound has its address taken";
        const char *why = nullptr;
        module.walk([&](cir::GetGlobalOp use) {
            if (!why && use.getName() == gg.getName()) why = storesSettled(use.getResult());
        });
        return why;
    }

    // `bound <= first + 1` (`bound <= first` for an inclusive loop), read
    // just before the constructor: whether the loop will run at most once.
    static Value atMostOnce(OpBuilder &b, Location loc, const SideBound &sb) {
        Value addr = sb.addr;
        if (auto gg = addr.getDefiningOp<cir::GetGlobalOp>()) addr = b.clone(*gg)->getResult(0);
        auto n = cir::LoadOp::create(b, loc, addr);
        auto lim = cir::ConstantOp::create(
            b, loc, n.getType(), cir::IntAttr::get(n.getType(), sb.l.first + (sb.l.inclusive ? 0 : 1)));
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
    static Operation *viaNumbered(ModuleOp module, IntegerAttr id) {
        Operation *found = nullptr;
        if (!id) return found;
        module.walk([&](cir::CallOp c) {
            if (auto a = c->getAttrOfType<IntegerAttr>(kSpawnViaAttrName); a && a.getInt() == id.getInt())
                found = c.getOperation();
        });
        return found;
    }

    // The one call, in `anchor`, of the function that holds the queue's
    // constructor, when that function builds the queue at most once per
    // call: the constructor in no loop, the function calling nothing that
    // could call it again. Null with the reason otherwise.
    static cir::CallOp builderCall(cir::CallOp ctor, cir::FuncOp anchor, const char *&why) {
        auto inner = ctor->getParentOfType<cir::FuncOp>();
        cir::CallOp found;
        unsigned n = 0;
        anchor.walk([&](cir::CallOp c) {
            if (lookupCallee(c) == inner) { found = c; ++n; }
        });
        if (n == 0) { why = "the queue is built outside the function whose loop starts its threads"; return {}; }
        if (n != 1) {
            why = "the function that builds the queue is called more than once by the one whose loop starts its threads";
            return {};
        }
        for (Operation *p = ctor->getParentOp(); p && p != inner.getOperation(); p = p->getParentOp())
            if (isa<cir::LoopOpInterface>(p)) { why = "the queue is built in a loop of the function that builds it"; return {}; }
        bool recursive = false;
        inner.walk([&](cir::CallOp c) { if (lookupCallee(c) == inner) recursive = true; });
        if (recursive) { why = "the function that builds the queue calls itself"; return {}; }
        if (inner.getAliasee() || inner.getBody().empty()) { why = "the function that builds the queue has no body here"; return {}; }
        return found;
    }

    // A copy of the function holding `ctor`, with the tag as one more
    // parameter that its copy of the constructor passes; `call`, its one
    // caller that counts, is redirected to the copy, passing `tag`. The
    // copy's constructor is no queue's root: the original stays the
    // queue's, with its tag, for every other caller.
    static void passTagThrough(cir::CallOp ctor, cir::CallOp call, Value tag) {
        auto inner = ctor->getParentOfType<cir::FuncOp>();
        auto module = inner->getParentOfType<ModuleOp>();
        IRMapping map;
        auto copy = cast<cir::FuncOp>(inner->clone(map));
        SymbolTable symbols(module);
        copy.setSymName((inner.getSymName() + ".wtc_tag").str());
        symbols.insert(copy, std::next(Block::iterator(inner.getOperation())));   // renames on a clash
        copy.setLinkage(cir::GlobalLinkageKind::InternalLinkage);
        copy.setComdat(false);
        copy->removeAttr("func_info");
        copy->removeAttr("sym_visibility");
        Type tagTy = tag.getType();
        llvm::SmallVector<Type, 8> inputs(copy.getFunctionType().getInputs());
        inputs.push_back(tagTy);
        cir::FuncType ft = copy.getFunctionType();
        copy.setFunctionType(cir::FuncType::get(copy.getContext(), inputs, ft.getOptionalReturnType(), ft.isVarArg()));
        if (ArrayAttr args = copy.getArgAttrsAttr()) {
            llvm::SmallVector<Attribute, 8> all(args.begin(), args.end());
            all.push_back(DictionaryAttr::get(copy.getContext()));
            copy.setArgAttrsAttr(ArrayAttr::get(copy.getContext(), all));
        }
        BlockArgument param = copy.getBody().front().addArgument(tagTy, ctor.getLoc());
        auto copied = cast<cir::CallOp>(map.lookup(ctor.getOperation()));
        copied->setOperand(1, param);
        for (llvm::StringRef a : {kTopologyAttrName.data(), kQueueIdAttrName.data(), kQueueNameAttrName.data(),
                                  kProducersLoopAttrName.data(), kConsumersLoopAttrName.data(),
                                  kProducersViaAttrName.data(), kConsumersViaAttrName.data(),
                                  kBorrowedAttrName.data(), kSpecializedAttrName.data()})
            copied->removeAttr(a);
        call.setCalleeAttr(FlatSymbolRefAttr::get(copy.getSymNameAttr()));
        call->insertOperands(call->getNumOperands(), {tag});
        if (ArrayAttr args = call.getArgAttrsAttr()) {
            llvm::SmallVector<Attribute, 8> all(args.begin(), args.end());
            all.push_back(DictionaryAttr::get(call.getContext()));
            call.setArgAttrsAttr(ArrayAttr::get(call.getContext(), all));
        }
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
        if (!takesTopologyTag(ctor)) return false;
        auto tagConst = ctor.getOperand(1).getDefiningOp<cir::ConstantOp>();
        auto tagVal = tagConst ? dyn_cast<cir::IntAttr>(tagConst.getValue()) : cir::IntAttr();
        if (!tagVal || tagVal.getSInt() != 0) return false;

        std::optional<SideBound> pl, cl;
        for (auto [id, viaAttr, out] :
             {std::tuple{pId, kProducersViaAttrName, &pl}, std::tuple{cId, kConsumersViaAttrName, &cl}}) {
            if (!id) continue;
            cir::ForOp loop = loopNumbered(module, id);
            std::optional<CountedLoop> l = loop ? countedLoop(loop) : std::nullopt;
            if (!l) return false;
            SideBound b{*l, l->bound.getAddr(), loop.getOperation()};
            if (auto viaId = root->getAttrOfType<IntegerAttr>(viaAttr)) {
                Operation *via = viaNumbered(module, viaId);
                if (!via) return false;
                if (const char *why = boundThrough(*l, via, b)) {
                    ctor.emitRemark() << "wtc: not specialised at run time: " << why;
                    return false;
                }
            }
            *out = b;
        }
        // Where the choice is made: the constructor, or the one call of the
        // function holding it from the function that runs the loops.
        auto anchor = (pl ? pl->at : cl->at)->getParentOfType<cir::FuncOp>();
        if (pl && cl && cl->at->getParentOfType<cir::FuncOp>() != anchor) {
            ctor.emitRemark() << "wtc: not specialised at run time: the loops that start its threads are run "
                                 "by different functions";
            return false;
        }
        Operation *site = ctor.getOperation();
        cir::CallOp builder;
        if (ctor->getParentOfType<cir::FuncOp>() != anchor) {
            const char *why = nullptr;
            builder = builderCall(ctor, anchor, why);
            if (!builder) {
                ctor.emitRemark() << "wtc: not specialised at run time: " << why;
                return false;
            }
            site = builder.getOperation();
        }
        for (const std::optional<SideBound> *b : {&pl, &cl}) {
            if (!*b) continue;
            if (const char *why = boundUnsettled(site, **b, module)) {
                ctor.emitRemark() << "wtc: not specialised at run time: " << why;
                return false;
            }
        }
        const bool manyP = verdict == Topology::mpsc || verdict == Topology::mpmc;
        const bool manyC = verdict == Topology::spmc || verdict == Topology::mpmc;
        const bool inlineOk = root->hasAttr(kBorrowedAttrName) && marksInlineOk(lookupCallee(ctor));

        OpBuilder b(site);
        const Location loc = site->getLoc();
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
        if (builder) passTagThrough(ctor, builder, tag);
        else ctor->setOperand(1, tag);
        root->setAttr(kSpecializedAttrName, TopologyAttr::get(&getContext(), verdict));

        // What each count gives, in the remark.
        auto where = [](const std::optional<SideBound> &l) {
            std::string w = whereOf(l->l.loop->getLoc());
            if (l->via) w += " (run by the call at " + whereOf(l->via->getLoc()) + ")";
            return w;
        };
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
        if (builder) d << "; chosen at the call at " << whereOf(builder.getLoc()) << ", which builds it";
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
            implOf[id.getInt()] = ctors.size() == 1 ? implOfTag(constructorTag(ctors), ctors.front()) : Impl::generic;
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
