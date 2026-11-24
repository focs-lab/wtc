#include "WTC/Transforms/ProgressPaths.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCInterfaces.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Interfaces/CIRLoopOpInterface.h"

#include "llvm/Demangle/Demangle.h"

using namespace mlir;
using namespace mlir::wtc;

namespace mlir::wtc {

Level LoopClassifier::levelOf(const LoopClass &c) {
    switch (c.kind) {
    case LoopKind::Counted:
    case LoopKind::SingleWriter:
    case LoopKind::Unknown:
        return Level::wait_free;
    case LoopKind::Retry:
        return Level::lock_free;
    case LoopKind::Wait:
        return Level::blocking_sf;
    case LoopKind::Dual:
        return Level::wait_free;  // the request; its follow-up is the pop's own level
    case LoopKind::Forever:
        return Level::blocking_sf;
    }
    return Level::blocking_sf;
}

void LoopClassifier::note(Evidence &ev, unsigned flag, Operation *op) {
    if (!(ev.flags & flag)) {
        if (flag == evCas || flag == evConstCas || flag == evHelperCas) ev.cas = op;
        if (flag == evRead) ev.read = op;
        if (flag == evPop) ev.pop = op;
        if (flag == evRmw) ev.rmw = op;
    }
    ev.flags |= flag;
}

// What anything a function reads or answers may depend on: every atomic
// and volatile access in it and in what it calls, every compare-and-swap
// and every pop. Coarse on purpose: a call's result is taken to depend on
// all of it.
unsigned LoopClassifier::summary(cir::FuncOp f) {
    if (!f || f.getBody().empty()) return 0;
    if (auto it = summaries.find(f); it != summaries.end()) return it->second;
    if (!inProgress.insert(f).second) {
        // Its own walk, further up, collects it; what is found below until
        // then is provisional and is not remembered.
        metCycle = true;
        return 0;
    }
    const bool outer = metCycle;
    metCycle = false;
    unsigned flags = 0;
    f.walk([&](Operation *op) {
        if (auto ld = dyn_cast<cir::LoadOp>(op)) {
            if (ld.getMemOrder() || ld.getIsVolatile()) flags |= evRead;
        } else if (isa<cir::AtomicCmpXchgOp>(op)) {
            flags |= evCas;
        } else if (isa<cir::AtomicFetchOp, cir::AtomicXchgOp, cir::AtomicTestAndSetOp>(op)) {
            flags |= evRmw;
        } else if (isa<QueuePopOp>(op) || (isa<QueuePushOp>(op) && op->getNumResults())) {
            flags |= evPop;  // whether a queue had an element, or room
        } else if (auto call = dyn_cast<cir::CallOp>(op)) {
            if (cir::FuncOp callee = lookupCallee(call)) flags |= summary(callee);
            else
                for (cir::FuncOp t : indirectCallees(call)) flags |= summary(t);
        }
    });
    // A compare-and-swap reached through a function of the program's own
    // (anything but std::atomic's members) has an expected value the loop
    // cannot read: a try_lock is a test-and-set.
    if (flags & evCas) {
        const std::string name = llvm::demangle(f.getSymName().str());
        if (!llvm::StringRef(name).starts_with("std::atomic") && !llvm::StringRef(name).starts_with("std::__atomic_base"))
            flags |= evHelperCas;
    }
    inProgress.erase(f);
    if (!metCycle) summaries[f] = flags;
    metCycle = outer || metCycle;
    return flags;
}

// Whether the loop stores a constant into a slot: the expected value of a
// compare-and-swap that the loop resets each time round is a spin lock's
// test-and-set, not a retry on a value it read.
bool LoopClassifier::setsConstantInside(Operation *loop, Value slot) {
    bool found = false;
    loop->walk([&](cir::StoreOp st) {
        if (st.getAddr() == slot && st.getValue().getDefiningOp<cir::ConstantOp>()) found = true;
    });
    return found;
}

// Whether the object of a compare-and-swap can be written by no other
// thread: a thread_local global, or a local variable whose address goes
// only to loads, stores and the members of std::atomic.
bool LoopClassifier::singleWriter(Value object) {
    if (!object) return false;
    Value root = object;
    for (;;) {
        root = traceToLocalRoot(root);
        if (auto up = root.getDefiningOp<cir::BaseClassAddrOp>()) {
            root = up.getDerivedAddr();
            continue;
        }
        break;
    }
    if (auto gg = root.getDefiningOp<cir::GetGlobalOp>()) {
        auto g = module.lookupSymbol<cir::GlobalOp>(gg.getName());
        return g && g.getTlsModel().has_value();
    }
    auto slot = root.getDefiningOp<cir::AllocaOp>();
    if (!slot) return false;
    llvm::SmallVector<Value, 4> work{slot.getResult()};
    while (!work.empty()) {
        Value v = work.pop_back_val();
        for (Operation *u : v.getUsers()) {
            if (isa<cir::BaseClassAddrOp, cir::CastOp>(u)) {
                work.push_back(u->getResult(0));
                continue;
            }
            // Its contents copied in or out: the address goes nowhere.
            if (isa<cir::LoadOp, cir::CopyOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
            if (auto st = dyn_cast<cir::StoreOp>(u)) {
                if (st.getValue() == v) return false;  // the address itself is stored
                continue;
            }
            auto call = dyn_cast<cir::CallOp>(u);
            cir::FuncOp callee = call ? lookupCallee(call) : cir::FuncOp();
            if (!callee) return false;
            const std::string name = llvm::demangle(callee.getSymName().str());
            if (!llvm::StringRef(name).starts_with("std::atomic") &&
                !llvm::StringRef(name).starts_with("std::__atomic_base"))
                return false;
        }
    }
    return true;
}

// The backward slice of v inside the loop, collecting what it depends on.
void LoopClassifier::slice(Operation *loop, Value v, Evidence &ev, llvm::DenseSet<Value> &seen, bool residual) {
    if (!v || !seen.insert(v).second) return;
    Operation *def = v.getDefiningOp();
    if (!def || !loop->isAncestor(def) || isa<cir::AllocaOp>(def)) {
        // A value from before the loop is the same each time round, except
        // a variable the loop writes, declared before it or inside it: what
        // the loop stores into it counts.
        if (auto slot = v.getDefiningOp<cir::AllocaOp>()) {
            loop->walk([&](Operation *op) {
                if (auto st = dyn_cast<cir::StoreOp>(op); st && st.getAddr() == v)
                    slice(loop, st.getValue(), ev, seen, residual);
                // A call handed the variable may write it: its result is
                // what it read.
                if (auto call = dyn_cast<cir::CallOp>(op))
                    if (llvm::is_contained(call.getOperands(), v)) {
                        if (cir::FuncOp callee = lookupCallee(call)) {
                            const unsigned s = summary(callee);
                            // A compare-and-swap writes back the value it
                            // found: what that depends on is the retry's.
                            if ((s & evCas) && residual) return;
                            if (s & evCas) note(ev, setsConstantInside(loop, v) ? evConstCas : evCas, call);
                            if (s & evHelperCas) note(ev, evHelperCas, call);
                            if (s & evRead) note(ev, evRead, call);
                            if (s & evRmw) note(ev, evRmw, call);
                            if (s & evPop) note(ev, evPop, call);
                        } else {
                            // Through a pointer: whatever it may reach.
                            for (cir::FuncOp t : indirectCallees(call)) {
                                const unsigned s = summary(t);
                                if (s & (evCas | evHelperCas)) note(ev, evHelperCas, call);
                                if (s & evRead) note(ev, evRead, call);
                                if (s & evRmw) note(ev, evRmw, call);
                                if (s & evPop) note(ev, evPop, call);
                            }
                        }
                    }
                // So may a pop that moves its element into it.
                if (auto pop = dyn_cast<QueuePopOp>(op); pop && pop.getOut() == v) note(ev, evPop, pop);
            });
        }
        return;
    }
    if (auto ld = dyn_cast<cir::LoadOp>(def)) {
        if (ld.getMemOrder() || ld.getIsVolatile()) note(ev, evRead, ld);
        // Through the variable to what the loop stored there.
        slice(loop, ld.getAddr(), ev, seen, residual);
        return;
    }
    if (auto cas = dyn_cast<cir::AtomicCmpXchgOp>(def)) {
        if (residual) return;
        Value expected = cas.getExpected();
        bool constant = expected.getDefiningOp<cir::ConstantOp>() != nullptr;
        if (auto ld = expected.getDefiningOp<cir::LoadOp>())
            constant |= setsConstantInside(loop, ld.getAddr());
        note(ev, constant ? evConstCas : evCas, cas);
        if (!ev.casObject) ev.casObject = cas.getPtr();
        return;
    }
    if (isa<cir::AtomicFetchOp, cir::AtomicXchgOp, cir::AtomicTestAndSetOp>(def)) {
        note(ev, evRmw, def);
        return;
    }
    if (auto pop = dyn_cast<QueuePopOp>(def)) {
        note(ev, evPop, pop);
        return;
    }
    // A push that may decline (a bounded queue's try_push): whether there
    // was room, which another thread's pop makes.
    if (auto push = dyn_cast<QueuePushOp>(def)) {
        note(ev, evPop, push);
        return;
    }
    if (auto call = dyn_cast<cir::CallOp>(def)) {
        llvm::SmallVector<cir::FuncOp, 4> targets;
        if (cir::FuncOp callee = lookupCallee(call)) targets.push_back(callee);
        else targets = indirectCallees(call);
        unsigned s = 0;
        for (cir::FuncOp t : targets) s |= summary(t);
        if (!lookupCallee(call) && (s & evCas)) s |= evHelperCas;
        if ((s & evCas) && residual) return;
        if (s & evHelperCas) note(ev, evHelperCas, call);
        if (s & evCas) {
            // compare_exchange(expected, desired) is handed the expected
            // value's variable; one the loop resets to a constant is a
            // test-and-set.
            bool constant = false;
            for (Value a : call.getArgOperands())
                if (a.getDefiningOp<cir::AllocaOp>() && setsConstantInside(loop, a)) constant = true;
            note(ev, constant ? evConstCas : evCas, call);
            if (!ev.casObject && call.getNumOperands() > 0) ev.casObject = call.getOperand(0);
        }
        if (s & evRead) note(ev, evRead, call);
        if (s & evRmw) note(ev, evRmw, call);
        if (s & evPop) note(ev, evPop, call);
        for (Value a : call.getArgOperands()) slice(loop, a, ev, seen, residual);
        return;
    }
    // Anything else: its operands, and what its regions yield (a ternary,
    // which is how && and || are spelled).
    for (Value o : def->getOperands()) slice(loop, o, ev, seen, residual);
    for (Region &r : def->getRegions())
        r.walk([&](cir::YieldOp y) {
            for (Value o : y.getOperands()) slice(loop, o, ev, seen, residual);
        });
}

LoopClass LoopClassifier::decide(const Evidence &ev, Operation *at) {
    LoopClass c;
    if ((ev.flags & evCas) && !(ev.flags & (evConstCas | evHelperCas))) {
        if (singleWriter(ev.casObject)) {
            c.kind = LoopKind::SingleWriter;
            c.trips = Count::one();
            c.why = "retries a compare-and-swap on a location no other thread writes, so it never fails";
        } else {
            c.kind = LoopKind::Retry;
            c.why = "retries a compare-and-swap that fails only when another thread changed the location: lock-free";
        }
        c.evidence = ev.cas;
        return c;
    }
    if (ev.flags & evPop) {
        c.kind = LoopKind::Dual;
        c.why = "waits for an element of a queue, or for room in one: a dual wait, for data, not for an operation in "
                "progress";
        c.evidence = ev.pop;
        return c;
    }
    if (ev.flags & (evRead | evRmw | evConstCas | evHelperCas)) {
        c.kind = LoopKind::Wait;
        Operation *e = (ev.flags & (evConstCas | evHelperCas)) ? ev.cas : (ev.flags & evRead) ? ev.read : ev.rmw;
        const char *what =
            (ev.flags & evConstCas)    ? "a compare-and-swap of an expected value it sets itself, a spin lock"
            : (ev.flags & evHelperCas) ? "a compare-and-swap inside a helper, which the check cannot tell from a "
                                         "spin lock's test-and-set"
            : (ev.flags & evRead)      ? "an atomic or volatile load"
                                       : "an exchange or a fetch-and-op";
        c.why = std::string("spins until another thread changes what it reads (") + what +
                "): it waits for another thread";
        c.evidence = e;
        return c;
    }
    c.kind = LoopKind::Unknown;
    c.why = "depends only on the thread's own data: finite, with no bound read";
    (void)at;
    return c;
}

LoopClass LoopClassifier::classify(Operation *loop) {
    if (auto f = dyn_cast<cir::ForOp>(loop)) {
        Count trips = loopTrips(f);
        if (!trips.isUnbounded()) {
            LoopClass c;
            c.kind = LoopKind::Counted;
            c.trips = trips;
            c.why = "runs " + trips.str() + " times at most";
            return c;
        }
    }
    if (!isa<cir::LoopOpInterface>(loop)) return {};
    Evidence ev;
    bool none = false;
    exits(loop, ev, /*residual=*/false, none);
    if (none) {
        // No exit: it never completes. What its body polls says whether it
        // waits for data or for another thread on the way.
        Evidence body;
        llvm::DenseSet<Value> seen;
        loop->walk([&](Operation *op) {
            if (auto i = dyn_cast<cir::IfOp>(op)) slice(loop, i.getCondition(), body, seen);
            else if (auto sw = dyn_cast<cir::SwitchOp>(op)) slice(loop, sw.getCondition(), body, seen);
        });
        LoopClass c;
        c.kind = LoopKind::Forever;
        c.why = (body.flags & evPop) ? "polls a queue in a loop it never leaves: a wait for data that never ends"
                : body.flags         ? "spins, in a loop it never leaves, on what other threads write"
                                     : "never leaves the loop: the path does not complete";
        c.evidence = (body.flags & evPop) ? body.pop : nullptr;
        return c;
    }
    LoopClass c = decide(ev, loop);
    if (c.kind == LoopKind::Retry || c.kind == LoopKind::SingleWriter) {
        // A retry only if nothing but the compare-and-swap decides: what
        // the exits depend on besides it, and what decides whether it
        // continues past the compare-and-swap, must read nothing another
        // thread writes. A test-and-test-and-set fails here.
        Evidence rest;
        bool unused = false;
        exits(loop, rest, /*residual=*/true, unused);
        if (rest.flags & (evRead | evRmw | evPop | evConstCas | evHelperCas)) {
            c.kind = LoopKind::Wait;
            c.trips = Count::unbounded();
            c.why = "retries a compare-and-swap only when what it read says the location is free, and otherwise "
                    "skips it: it waits on the value it read, as a spin lock does";
            c.evidence = rest.read ? rest.read : rest.rmw ? rest.rmw : c.evidence;
        }
    }
    return c;
}

void LoopClassifier::exits(Operation *loop, Evidence &ev, bool residual, bool &none) {
    auto iface = cast<cir::LoopOpInterface>(loop);
    llvm::DenseSet<Value> seen;
    // The condition; a constant true one is no exit.
    bool always = false;
    iface.getCond().walk([&](cir::ConditionOp cond) {
        Value v = cond.getCondition();
        if (auto k = v.getDefiningOp<cir::ConstantOp>())
            if (auto b = dyn_cast<cir::BoolAttr>(k.getValue()); b && b.getValue()) always = true;
        slice(loop, v, ev, seen, residual);
    });
    // A break out of this loop, a return from inside it, and a continue of
    // it, under the conditions of the ifs and switches around them.
    bool leaves = false;
    loop->walk([&](Operation *op) {
        if (!isa<cir::BreakOp, cir::ReturnOp, cir::ContinueOp>(op)) return;
        if (isa<cir::BreakOp, cir::ContinueOp>(op)) {
            Operation *p = op->getParentOp();
            // A break inside a switch leaves the switch, not the loop.
            while (p && !isa<cir::LoopOpInterface>(p) && !(isa<cir::BreakOp>(op) && isa<cir::SwitchOp>(p)))
                p = p->getParentOp();
            if (p != loop) return;
        }
        if (!isa<cir::ContinueOp>(op)) leaves = true;
        for (Operation *p = op->getParentOp(); p && p != loop; p = p->getParentOp()) {
            if (auto ifOp = dyn_cast<cir::IfOp>(p)) slice(loop, ifOp.getCondition(), ev, seen, residual);
            if (auto sw = dyn_cast<cir::SwitchOp>(p)) slice(loop, sw.getCondition(), ev, seen, residual);
        }
    });
    none = always && !leaves;
}

LoopClass LoopClassifier::classifyGoto(cir::GotoOp go) {
    auto f = go->getParentOfType<cir::FuncOp>();
    Evidence ev;
    const unsigned s = summary(f);
    if (s & evCas) note(ev, evCas, go);
    if (s & evRead) note(ev, evRead, go);
    if (s & evRmw) note(ev, evRmw, go);
    if (s & evPop) note(ev, evPop, go);
    LoopClass c = decide(ev, go);
    // The function's own code is all a goto can loop over: a retry is not
    // read through one, and any synchronisation in reach makes it a wait.
    if (c.kind == LoopKind::Retry || c.kind == LoopKind::SingleWriter) {
        c.kind = LoopKind::Wait;
        c.why = "jumps by goto in a function that synchronises, which the check cannot read as a loop: counted as "
                "a wait";
    } else if (c.kind == LoopKind::Wait) {
        c.why = "jumps by goto in a function that reads what other threads write, which the check cannot read as a "
                "loop: counted as a wait";
    } else if (c.kind == LoopKind::Dual) {
        c.why = "jumps by goto in a function that pops a queue, which the check cannot read as a loop: counted as a "
                "dual wait";
    }
    return c;
}

} // namespace mlir::wtc

// ---------------------------------------------------------------------------
// Composing a path

namespace mlir::wtc {

PathComposer::PathComposer(ModuleOp module) : module(module), classifier(module), elements(module) {
    module.walk([&](Operation *op) {
        if (auto id = op->getAttrOfType<IntegerAttr>(kQueueIdAttrName))
            if (op->hasAttr(kTopologyAttrName)) roots.try_emplace(id.getInt(), op);
        if (auto pop = dyn_cast<QueuePopOp>(op))
            if (pop.getBorrowed() && pop.getImpl() == Impl::spsc_inline && pop.getOut())
                ownerlessSlots.insert(pop.getOut());
    });
}

ThreadCounts PathComposer::countsOf(Operation *op) const {
    auto id = op->getAttrOfType<IntegerAttr>("queue_id");
    if (!id) return ThreadCounts::unknown();
    Operation *root = roots.lookup(id.getInt());
    if (!root) return ThreadCounts::unknown();
    ThreadCounts c;
    if (auto p = root->getAttrOfType<StringAttr>(kProducersCountAttrName)) c.producers = Count::parse(p.getValue());
    if (auto k = root->getAttrOfType<StringAttr>(kConsumersCountAttrName)) c.consumers = Count::parse(k.getValue());
    return c;
}

std::string PathComposer::queueName(Operation *op) const {
    auto id = op->getAttrOfType<IntegerAttr>("queue_id");
    Operation *root = id ? roots.lookup(id.getInt()) : nullptr;
    if (!root) return "its queue";
    if (auto n = root->getAttrOfType<StringAttr>(kQueueNameAttrName)) return n.getValue().str();
    if (auto g = dyn_cast<cir::GlobalOp>(root)) return "@" + g.getSymName().str();
    if (auto a = dyn_cast<cir::AllocaOp>(root)) return "'" + a.getName().str() + "'";
    return "its queue";
}

PathProgress PathComposer::blocked(Operation *at, Level level, std::string why) {
    PathProgress p;
    p.level = level;
    p.bound = p.amortised = Bound::none();
    p.weakest = at;
    p.weakWhy = std::move(why);
    return p;
}

PathProgress PathComposer::seq(PathProgress a, const PathProgress &b) {
    if (b.level < a.level || (!a.weakest && b.weakest)) {
        a.weakest = b.weakest;
        a.weakWhy = b.weakWhy;
    }
    a.level = std::min(a.level, b.level);
    a.partial |= b.partial;
    a.bound = a.bound.plus(b.bound);
    a.amortised = a.amortised.plus(b.amortised);
    if (static_cast<int>(b.alloc.kind) > static_cast<int>(a.alloc.kind)) a.alloc = b.alloc;
    a.kernel = std::max(a.kernel, b.kernel);
    a.firstUse |= b.firstUse;
    for (const std::string &c : b.counts)
        if (!llvm::is_contained(a.counts, c)) a.counts.push_back(c);
    return a;
}

PathProgress PathComposer::alt(PathProgress a, const PathProgress &b) {
    PathProgress r = seq(a, b);
    r.bound = a.bound.max(b.bound);
    r.amortised = a.amortised.max(b.amortised);
    return r;
}

PathProgress PathComposer::region(Region &r) {
    PathProgress p;
    // A region of several blocks has control flow its operations do not
    // show (a goto): every block may run, and the gotos are read as loops
    // where they are met.
    for (Block &b : r)
        for (Operation &o : b) p = seq(p, op(&o));
    return p;
}

PathProgress PathComposer::enter(cir::FuncOp f) {
    if (auto it = memo.find(f); it != memo.end()) return it->second;
    if (!inProgress.insert(f).second) {
        // A recursion: its level is the one its own walk finds further up,
        // and how deep it goes is not read. What is composed below until
        // that walk ends is provisional and is not remembered.
        metCycle = true;
        PathProgress p;
        p.bound = p.amortised = Bound::finite("a recursion");
        return p;
    }
    const bool outer = metCycle;
    metCycle = false;
    PathProgress p = region(f.getBody());
    inProgress.erase(f);
    if (!metCycle) memo[f] = p;
    metCycle = outer || metCycle;
    return p;
}

PathProgress PathComposer::call(cir::CallOp c) {
    llvm::SmallVector<cir::FuncOp, 4> targets;
    if (cir::FuncOp callee = lookupCallee(c)) {
        // A member of the shared_ptr a borrowed pop of an inline queue
        // returns: it owns nothing, and its members free nothing.
        if (c.getNumOperands() > 0) {
            Value self = c.getOperand(0);
            while (auto up = self.getDefiningOp<cir::BaseClassAddrOp>()) self = up.getDerivedAddr();
            if (ownerlessSlots.contains(self)) return PathProgress{};
        }
        targets.push_back(callee);
    } else {
        targets = indirectCallees(c);
        if (targets.empty())
            return blocked(c, Level::blocking_df, "makes an indirect call whose target is not known here");
    }
    std::optional<PathProgress> out;
    for (cir::FuncOp t : targets) {
        PathProgress p;
        if (t.getBody().empty() || cir::isInterposableLinkage(t.getLinkage())) {
            if (auto ext = externalBlocking(t)) {
                if (ext->kind == BlockingKind::MayThrow) continue;
                p = blocked(c, Level::blocking_df, ext->what);
                if (ext->kind == BlockingKind::Allocates) p.alloc = Alloc::always();
            }
        } else {
            p = enter(t);
        }
        out = out ? alt(*out, p) : p;
    }
    return out ? *out : PathProgress{};
}

PathProgress PathComposer::loop(Operation *l) {
    // The condition runs once more than the body and the step.
    auto iface = cast<cir::LoopOpInterface>(l);
    PathProgress cond = region(iface.getCond()), rest;
    for (Region &r : l->getRegions())
        if (&r != &iface.getCond()) rest = seq(rest, region(r));
    PathProgress body = seq(cond, rest);
    const LoopClass c = classifier.classify(l);
    switch (c.kind) {
    case LoopKind::Counted:
    case LoopKind::SingleWriter: {
        PathProgress p = body;
        p.bound = rest.bound.times(c.trips, "a loop's bound").plus(cond.bound.times(c.trips + Count::one(), "a loop's bound"));
        p.amortised =
            rest.amortised.times(c.trips, "a loop's bound").plus(cond.amortised.times(c.trips + Count::one(), "a loop's bound"));
        if (!p.bound.isPoly() && !p.weakest) {
            p.weakest = l;
            p.weakWhy = c.why;
        }
        return p;
    }
    case LoopKind::Retry:
    case LoopKind::Wait:
    case LoopKind::Forever: {
        PathProgress p = seq(body, blocked(l, LoopClassifier::levelOf(c), c.why));
        p.bound = p.amortised = Bound::none();
        return p;
    }
    case LoopKind::Dual: {
        // A wait for data: the request; each attempt takes the body's steps.
        PathProgress p = body;
        p.partial = true;
        if (!p.weakest) {
            p.weakest = l;
            p.weakWhy = c.why;
        }
        return p;
    }
    case LoopKind::Unknown: {
        PathProgress p = body;
        const bool none = body.bound.isPoly() && body.bound.terms.empty();
        if (!none) {
            p.bound = body.bound.isPoly() ? Bound::finite("a loop over the thread's own data") : body.bound;
            p.amortised = p.bound;
            if (!p.weakest) {
                p.weakest = l;
                p.weakWhy = c.why;
            }
        }
        return p;
    }
    }
    return body;
}

PathProgress PathComposer::op(Operation *o) {
    if (auto p = dyn_cast<ProgressOpInterface>(o)) {
        const ThreadCounts counts = countsOf(o);
        ProgressSpec s = p.getProgressSpec(counts);
        PathProgress r;
        r.level = s.level;
        r.partial = s.partial;
        r.bound = s.level == Level::wait_free ? s.bound.substitute(counts) : Bound::none();
        r.amortised = s.amortised.kind == Bound::Kind::None ? r.bound : s.amortised.substitute(counts);
        r.alloc = s.alloc;
        r.kernel = s.kernel;
        r.firstUse = s.firstUse;
        if (s.level < Level::wait_free || s.partial || !s.bound.isPoly()) {
            r.weakest = o;
            r.weakWhy = !s.reason.empty() ? s.reason
                        : s.bound.isPoly() || s.level < Level::wait_free ? std::string(describeLevel(s.level))
                                                                          : "its bound is " + s.bound.str();
        }
        if (s.bound.dependsOnCounts() || s.amortised.dependsOnCounts()) {
            const std::string q = queueName(o);
            if (s.bound.terms.end() != llvm::find_if(s.bound.terms, [](const Term &t) { return t.np; }) ||
                llvm::any_of(s.amortised.terms, [](const Term &t) { return t.np; }))
                r.counts.push_back("np = " + counts.producers.str() + " for " + q);
            if (llvm::any_of(s.bound.terms, [](const Term &t) { return t.nc; }) ||
                llvm::any_of(s.amortised.terms, [](const Term &t) { return t.nc; }))
                r.counts.push_back("nc = " + counts.consumers.str() + " for " + q);
        }
        // The element's own code, as calls the operation makes.
        if (s.element) {
            ElementMembers::Found found = elements.of(o, s.element);
            if (found.unknown)
                r = seq(r, blocked(o, Level::blocking_df, "runs the element's code, and its type is not known here"));
            for (cir::FuncOp f : found.funcs) {
                PathProgress m;
                if (f.getBody().empty() || cir::isInterposableLinkage(f.getLinkage())) {
                    if (auto ext = externalBlocking(f)) m = blocked(o, Level::blocking_df, ext->what);
                } else {
                    m = enter(f);
                }
                r = seq(r, m);
            }
        }
        return r;
    }
    if (isa<cir::LoopOpInterface>(o)) return loop(o);
    if (auto go = dyn_cast<cir::GotoOp>(o)) {
        const LoopClass c = classifier.classifyGoto(go);
        if (c.kind == LoopKind::Wait) return blocked(o, Level::blocking_sf, c.why);
        PathProgress p;
        p.bound = p.amortised = Bound::finite("a goto");
        p.partial = c.kind == LoopKind::Dual;
        p.weakest = o;
        p.weakWhy = c.why;
        return p;
    }
    if (auto c = dyn_cast<cir::CallOp>(o)) return call(c);
    if (auto a = dyn_cast<cir::InlineAsmOp>(o)) {
        if (onlyNonBlockingInstructions(a.getAsmString())) {
            PathProgress p;
            p.bound = p.amortised = Bound::constant(1);
            return p;
        }
        return blocked(o, Level::blocking_df, "runs inline assembly, which is not looked into");
    }
    if (isa<cir::ThrowOp, cir::AllocExceptionOp>(o))
        return blocked(o, Level::blocking_df, "throws: the C++ runtime allocates the exception and unwinds the stack");
    // One step for each atomic access.
    PathProgress p;
    if (auto ld = dyn_cast<cir::LoadOp>(o); ld && (ld.getMemOrder() || ld.getIsVolatile()))
        p.bound = p.amortised = Bound::constant(1);
    else if (auto st = dyn_cast<cir::StoreOp>(o); st && (st.getMemOrder() || st.getIsVolatile()))
        p.bound = p.amortised = Bound::constant(1);
    else if (isa<cir::AtomicCmpXchgOp, cir::AtomicFetchOp, cir::AtomicXchgOp, cir::AtomicTestAndSetOp,
                 cir::AtomicClearOp, cir::AtomicFenceOp>(o))
        p.bound = p.amortised = Bound::constant(1);
    // Alternatives: the arms of an if and of a ternary, the cases of a
    // switch (which may fall through: summed, which bounds them).
    if (isa<cir::IfOp, cir::TernaryOp>(o)) {
        std::optional<PathProgress> arms;
        for (Region &r : o->getRegions()) {
            PathProgress a = region(r);
            arms = arms ? alt(*arms, a) : a;
        }
        return arms ? seq(p, *arms) : p;
    }
    for (Region &r : o->getRegions()) p = seq(p, region(r));
    return p;
}

PathProgress PathComposer::of(cir::FuncOp f) { return enter(f); }

} // namespace mlir::wtc
