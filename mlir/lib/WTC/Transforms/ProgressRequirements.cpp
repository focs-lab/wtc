#include "WTC/Transforms/ProgressRequirements.h"

#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"

#include "llvm/ADT/SetVector.h"
#include "llvm/Demangle/Demangle.h"

#include <algorithm>
#include <deque>
#include <functional>

namespace mlir::wtc {

std::string Requirement::describe() const {
    switch (kind) {
    case Kind::lock_free: return "lock-free";
    case Kind::wait_free:
        return within ? "wait-free within " + std::to_string(*within) + " steps" : "wait-free";
    case Kind::critical: return within ? "progress-critical within " + std::to_string(*within) + " steps" : "progress-critical";
    }
    return "lock-free";
}

bool Requirement::covers(const Requirement &o) const {
    if (kind < o.kind) return false;
    if (o.within && (!within || *within > *o.within)) return false;
    return true;
}

// ClangIR does not carry an annotation's arguments, so the bound of
// WTC_WAIT_FREE_WITHIN(k) is spelled in its name:
// "wtc_progress_wait_free_within_<k>".
std::optional<Requirement> declaredRequirement(cir::FuncOp f) {
    auto annotations = f.getAnnotationsAttr();
    if (!annotations) return std::nullopt;
    std::optional<Requirement> out;
    auto take = [&](Requirement r) {
        if (!out) {
            out = r;
            return;
        }
        if (r.kind > out->kind) out->kind = r.kind;
        if (r.within && (!out->within || *r.within < *out->within)) out->within = r.within;
    };
    for (Attribute a : annotations) {
        auto ann = dyn_cast<cir::AnnotationAttr>(a);
        if (!ann) continue;
        llvm::StringRef name = ann.getName().getValue();
        if (name == "wtc_progress_lock_free") take({Requirement::Kind::lock_free, std::nullopt});
        else if (name == "wtc_progress_wait_free") take({Requirement::Kind::wait_free, std::nullopt});
        else if (name == "wtc_progress_critical") take({Requirement::Kind::critical, std::nullopt});
        else if (name.consume_front("wtc_progress_wait_free_within_")) {
            uint64_t k;
            // A bound that does not read as a number promises nothing a
            // check could hold it to: zero steps, which every path fails.
            if (name.getAsInteger(10, k)) k = 0;
            take({Requirement::Kind::wait_free, k});
        }
    }
    return out;
}

namespace {

// SCHED_FIFO and SCHED_RR on Linux.
constexpr int64_t kSchedFifo = 1, kSchedRr = 2;
// PTHREAD_EXPLICIT_SCHED in glibc.
constexpr int64_t kExplicitSched = 1;

std::optional<int64_t> constantOf(Value v) {
    while (auto c = v.getDefiningOp<cir::CastOp>()) v = c.getSrc();
    auto k = v.getDefiningOp<cir::ConstantOp>();
    auto i = k ? dyn_cast<cir::IntAttr>(k.getValue()) : cir::IntAttr();
    if (!i) return std::nullopt;
    return i.getSInt();
}

// The policy argument: a real-time one (in words), a time-sharing one, or
// one that is not a constant here. A policy that is not known is not taken
// for a requirement (decided 28 Nov): read as real-time, a policy taken
// from a configuration, set on main, made every function main reaches,
// and every thread it starts, wait-free-required, which refused batching
// and delegation and chose slower queues across the program. It is
// reported instead, so that the program can state the requirement on the
// thread it means (WTC_WAIT_FREE on its entry).
enum class Policy { realtime, other, unknown };
Policy policyOf(Value policy, std::string &what) {
    auto k = constantOf(policy);
    if (!k) return Policy::unknown;
    if (*k == kSchedFifo) what = "SCHED_FIFO";
    else if (*k == kSchedRr) what = "SCHED_RR";
    else return Policy::other;
    return Policy::realtime;
}

// The object a pointer operand points to: a slot or a global, through
// casts, member steps and base-class steps.
Value objectOf(Value v) {
    for (;;) {
        v = traceToLocalRoot(v);
        if (auto m = v.getDefiningOp<cir::GetMemberOp>()) {
            v = m.getAddr();
            continue;
        }
        if (auto b = v.getDefiningOp<cir::BaseClassAddrOp>()) {
            v = b.getDerivedAddr();
            continue;
        }
        return v;
    }
}

bool sameObject(Value a, Value b) {
    a = objectOf(a);
    b = objectOf(b);
    if (a == b) return true;
    auto ga = a.getDefiningOp<cir::GetGlobalOp>(), gb = b.getDefiningOp<cir::GetGlobalOp>();
    return ga && gb && ga.getName() == gb.getName();
}

// An object a pointer operand may name, as far as attributes are told
// apart: its root (a slot or a global, objectOf), the fields taken on the
// way from it (two members of one struct are two objects), and whether an
// element of an array was taken on the way, at an index this analysis does
// not read (the element is then any element of the array).
struct Obj {
    Value root;
    llvm::SmallVector<uint64_t, 2> fields;
    bool element = false;
};
Obj objWithPath(Value v) {
    Obj o;
    for (;;) {
        v = traceToLocalRoot(v);
        if (auto m = v.getDefiningOp<cir::GetMemberOp>()) {
            o.fields.insert(o.fields.begin(), m.getIndex());
            v = m.getAddr();
            continue;
        }
        if (auto b = v.getDefiningOp<cir::BaseClassAddrOp>()) {
            v = b.getDerivedAddr();
            continue;
        }
        if (auto ps = v.getDefiningOp<cir::PtrStrideOp>()) {
            o.element = true;
            o.fields.clear();
            v = ps.getBase();
            continue;
        }
        if (auto ge = v.getDefiningOp<cir::GetElementOp>()) {
            o.element = true;
            o.fields.clear();
            v = ge.getBase();
            continue;
        }
        o.root = v;
        return o;
    }
}

// The objects a pointer operand may name, followed through a parameter
// of the function it is read in to what every call of that function
// passes for it, a few calls deep: pthread_attr_t set up by a helper that
// main hands its own attributes to. `partial` is set when some of them
// cannot be named: a parameter of a function with external linkage,
// which another translation unit may call, or of one nothing here calls,
// or a pointer read from memory.
struct Objects {
    llvm::SmallVector<Obj, 2> objs;
    bool partial = false;
};
void objectsOf(Value v, Objects &out, unsigned depth = 0) {
    Obj o = objWithPath(v);
    Value r = o.root.getDefiningOp<cir::LoadOp>() ? traceToRoot(o.root) : o.root;
    auto arg = dyn_cast<BlockArgument>(r);
    auto f = arg ? dyn_cast<cir::FuncOp>(arg.getOwner()->getParentOp()) : cir::FuncOp();
    // What the caller's argument names, with the fields and element taken
    // here after it.
    auto extend = [&](Obj inner) {
        if (o.element) {
            inner.element = true;
            inner.fields.clear();
        } else if (!inner.element) {
            inner.fields.append(o.fields.begin(), o.fields.end());
        }
        out.objs.push_back(inner);
    };
    if (!f || arg.getOwner() != &f.getBody().front()) {
        if (o.root.getDefiningOp<cir::LoadOp>()) {
            if (!r.getDefiningOp<cir::AllocaOp>() && !r.getDefiningOp<cir::GetGlobalOp>()) out.partial = true;
            extend(objWithPath(r));
        } else {
            out.objs.push_back(o);
        }
        return;
    }
    if (depth >= 4 || !cir::isLocalLinkage(f.getLinkage())) out.partial = true;
    if (depth >= 4) return;
    bool called = false;
    f->getParentOfType<ModuleOp>().walk([&](cir::CallOp call) {
        if (lookupCallee(call) != f || arg.getArgNumber() >= call.getNumOperands()) return;
        called = true;
        Objects inner;
        objectsOf(call.getOperand(arg.getArgNumber()), inner, depth + 1);
        out.partial |= inner.partial;
        for (Obj &x : inner.objs) extend(x);
    });
    if (!called) out.partial = true;
}
Objects objectsOf(Value v) {
    Objects out;
    objectsOf(v, out);
    return out;
}
bool sameRoot(Value a, Value b) {
    if (a == b) return true;
    auto ga = a.getDefiningOp<cir::GetGlobalOp>(), gb = b.getDefiningOp<cir::GetGlobalOp>();
    return ga && gb && ga.getName() == gb.getName();
}
// May the two name one object: one root, and fields of which one is a
// prefix of the other, or an element of an array on either side.
bool mayBeSame(const Obj &a, const Obj &b) {
    if (!sameRoot(a.root, b.root)) return false;
    if (a.element || b.element) return true;
    const size_t n = std::min(a.fields.size(), b.fields.size());
    return std::equal(a.fields.begin(), a.fields.begin() + n, b.fields.begin());
}
bool mayBeSame(const Objects &a, const Objects &b) {
    for (const Obj &x : a.objs)
        for (const Obj &y : b.objs)
            if (mayBeSame(x, y)) return true;
    return false;
}
// Must the two name one object: each is one object, named in full, with
// one root and the same fields, and no element of an array.
bool mustBeSame(const Objects &a, const Objects &b) {
    if (a.partial || b.partial || a.objs.size() != 1 || b.objs.size() != 1) return false;
    const Obj &x = a.objs.front(), &y = b.objs.front();
    return !x.element && !y.element && sameRoot(x.root, y.root) && x.fields == y.fields;
}

// Every function stored into the object: the handlers a struct sigaction
// is given, in the function that hands it over, or anywhere for a global.
llvm::SmallVector<cir::FuncOp, 2> functionsStoredInto(Value object, ModuleOp module, bool &other) {
    llvm::SmallVector<cir::FuncOp, 2> out;
    other = false;
    Operation *scope = object.getDefiningOp<cir::AllocaOp>()
                           ? object.getDefiningOp()->getParentOfType<cir::FuncOp>().getOperation()
                           : module.getOperation();
    scope->walk([&](cir::StoreOp st) {
        if (!sameObject(st.getAddr(), object)) return;
        if (!isa<cir::PointerType>(st.getValue().getType())) return;
        if (cir::FuncOp f = resolveFunctionOperand(traceToRoot(st.getValue()))) {
            if (!llvm::is_contained(out, f)) out.push_back(f);
            return;
        }
        // A pointer that is not a constant: SIG_IGN and SIG_DFL are
        // integers cast to pointers, and so harmless; anything else is a
        // handler the analysis cannot name.
        Value v = st.getValue();
        while (auto c = v.getDefiningOp<cir::CastOp>()) v = c.getSrc();
        if (!v.getDefiningOp<cir::ConstantOp>()) other = true;
    });
    return out;
}

} // namespace

void RequirementRoots::addHandler(cir::FuncOp h, Operation *origin, llvm::StringRef how) {
    if (!h || h.getBody().empty()) return;
    for (const RequirementRoot &r : roots_)
        if (r.func == h && r.source == RequirementRoot::Source::handler) return;
    RequirementRoot r;
    r.func = h;
    r.req = {Requirement::Kind::lock_free, std::nullopt};
    r.source = RequirementRoot::Source::handler;
    r.origin = origin;
    r.why = ("a signal handler, installed by " + how).str();
    roots_.push_back(r);
}

void RequirementRoots::addRealtime(cir::FuncOp entry, Operation *origin, llvm::StringRef how) {
    if (!entry || entry.getBody().empty()) return;
    for (const RequirementRoot &r : roots_)
        if (r.func == entry && r.source == RequirementRoot::Source::realtime) return;
    RequirementRoot r;
    r.func = entry;
    r.req = {Requirement::Kind::wait_free, std::nullopt};
    r.source = RequirementRoot::Source::realtime;
    r.origin = origin;
    r.why = ("a thread at real-time priority (" + how + ")").str();
    roots_.push_back(r);
}

RequirementRoots::RequirementRoots(ModuleOp module) : module(module) {
    for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
        if (f.getBody().empty()) continue;
        if (auto req = declaredRequirement(f)) {
            RequirementRoot r;
            r.func = f;
            r.req = *req;
            r.why = "declared " + req->describe();
            roots_.push_back(r);
        }
    }
    // The threads pthread_create starts, by the slot their id is written to
    // and by the attributes they are handed.
    llvm::SmallVector<cir::CallOp, 4> creates;
    module.walk([&](cir::CallOp call) {
        if (cir::FuncOp callee = lookupCallee(call); callee && callee.getSymName() == "pthread_create")
            creates.push_back(call);
    });
    module.walk([&](cir::CallOp call) {
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return;
        const llvm::StringRef sym = callee.getSymName();
        const unsigned base = firstRealOperand(call);
        auto arg = [&](unsigned i) -> Value { return base + i < call.getNumOperands() ? call.getOperand(base + i) : Value(); };
        auto caller = call->getParentOfType<cir::FuncOp>();
        if (sym == "signal" || sym == "sysv_signal" || sym == "bsd_signal") {
            Value h = arg(1);
            if (!h) return;
            if (cir::FuncOp f = resolveFunctionOperand(traceToRoot(h))) addHandler(f, call, sym);
            else if (!constantOf(h)) unresolved_.push_back({call, "installs a signal handler the analysis cannot name"});
            return;
        }
        if (sym == "sigaction") {
            Value act = arg(1);
            if (!act || constantOf(act)) return;  // a query: no new handler
            bool other = false;
            auto hs = functionsStoredInto(objectOf(act), module, other);
            for (cir::FuncOp h : hs) addHandler(h, call, "sigaction");
            if (other || hs.empty()) unresolved_.push_back({call, "installs a signal handler the analysis cannot name"});
            return;
        }
        std::string what;
        auto policy = [&](Value p) {
            const Policy k = p ? policyOf(p, what) : Policy::other;
            if (k == Policy::unknown)
                unresolved_.push_back({call, "sets a scheduling policy not known here, which is not taken for a "
                                             "real-time requirement (state one with WTC_WAIT_FREE on the thread's "
                                             "entry)"});
            return k == Policy::realtime;
        };
        if (sym == "sched_setscheduler") {
            if (!policy(arg(1))) return;
            // Zero is the calling thread; any other process is not this one.
            auto pid = constantOf(arg(0));
            if (pid && *pid == 0) addRealtimeCaller(caller, call, what + ", sched_setscheduler");
            else unresolved_.push_back({call, "sets a real-time policy on a process the analysis cannot name"});
            return;
        }
        if (sym == "pthread_setschedparam") {
            if (!policy(arg(1))) return;
            Value t = arg(0);
            // pthread_self(): the calling thread, from here on.
            if (auto self = t.getDefiningOp<cir::CallOp>())
                if (cir::FuncOp sf = lookupCallee(self); sf && sf.getSymName() == "pthread_self") {
                    addRealtimeCaller(caller, call, what + ", pthread_setschedparam");
                    return;
                }
            // A thread the module started: its id read from the slot
            // pthread_create wrote.
            bool found = false;
            if (auto ld = t.getDefiningOp<cir::LoadOp>())
                for (cir::CallOp c : creates) {
                    const unsigned cb = firstRealOperand(c);
                    if (cb + 2 >= c.getNumOperands() || !sameObject(c.getOperand(cb), ld.getAddr())) continue;
                    if (cir::FuncOp e = resolveFunctionOperand(traceToRoot(c.getOperand(cb + 2)))) {
                        addRealtime(e, call, what + ", pthread_setschedparam");
                        found = true;
                    }
                }
            if (!found) unresolved_.push_back({call, "sets a real-time policy on a thread the analysis cannot name"});
            return;
        }
        if (sym == "pthread_attr_setschedpolicy") {
            if (!policy(arg(1))) return;
            const Objects attr = objectsOf(arg(0));
            if (attr.partial || attr.objs.empty())
                unresolved_.push_back({call, "sets a real-time policy on attributes the analysis cannot follow to "
                                             "every thread they start"});
            // Only explicit scheduling makes the new thread take the policy:
            // a pthread_attr_setinheritsched on attributes that may be these,
            // with PTHREAD_EXPLICIT_SCHED or a value not known here.
            bool explicitSched = false;
            module.walk([&](cir::CallOp c) {
                cir::FuncOp cf = lookupCallee(c);
                if (!cf || cf.getSymName() != "pthread_attr_setinheritsched") return;
                const unsigned cb = firstRealOperand(c);
                if (cb + 1 >= c.getNumOperands()) return;
                const Objects other = objectsOf(c.getOperand(cb));
                if (!other.partial && !mayBeSame(other, attr)) return;
                auto k = constantOf(c.getOperand(cb + 1));
                if (!k || *k == kExplicitSched) explicitSched = true;
            });
            if (!explicitSched) return;
            bool found = false;
            for (cir::CallOp c : creates) {
                const unsigned cb = firstRealOperand(c);
                if (cb + 2 >= c.getNumOperands() || !mayBeSame(objectsOf(c.getOperand(cb + 1)), attr)) continue;
                if (cir::FuncOp e = resolveFunctionOperand(traceToRoot(c.getOperand(cb + 2)))) {
                    addRealtime(e, call, what + ", pthread_attr_setschedpolicy");
                    found = true;
                }
            }
            if (!found && !attr.partial && !attr.objs.empty())
                unresolved_.push_back({call, "sets a real-time policy for threads the analysis cannot name"});
        }
    });
    computeReach();
}

// The thread a call runs on is the one some start routine began: the
// requirement of a thread that raises its own priority holds from that
// routine's entry, whichever helper on the way makes the call. Up the
// call graph from the caller to every function that starts a thread
// (pthread_create's and the shim's spawn entries, main) or that nothing
// here calls (a std::thread's _M_run, a callback): each is a real-time
// root.
void RequirementRoots::addRealtimeCaller(cir::FuncOp caller, Operation *origin, llvm::StringRef how) {
    if (callers_.empty()) {
        module.walk([&](cir::CallOp call) {
            auto from = call->getParentOfType<cir::FuncOp>();
            llvm::SmallVector<cir::FuncOp, 4> targets;
            if (cir::FuncOp callee = lookupCallee(call)) targets.push_back(callee);
            else targets = indirectCallees(call);
            for (cir::FuncOp t : targets) callers_[t].insert(from);
            if (cir::FuncOp e = spawnedEntry(call)) entries_.insert(e);
        });
        if (auto m = module.lookupSymbol<cir::FuncOp>("main")) entries_.insert(m);
    }
    llvm::DenseSet<Operation *> seen{caller};
    llvm::SmallVector<cir::FuncOp, 8> work{caller};
    while (!work.empty()) {
        cir::FuncOp f = work.pop_back_val();
        auto it = callers_.find(f);
        if (entries_.contains(f) || it == callers_.end() || it->second.empty()) {
            addRealtime(f, origin, how);
            if (it == callers_.end() || entries_.contains(f)) continue;
        }
        for (Operation *c : it->second)
            if (seen.insert(c).second) work.push_back(cast<cir::FuncOp>(c));
    }
}

// The function a call starts as a thread, when it is one the analysis can
// name: pthread_create's third argument, the shim's spawn's entry.
cir::FuncOp RequirementRoots::spawnedEntry(cir::CallOp call) {
    cir::FuncOp callee = lookupCallee(call);
    if (!callee) return cir::FuncOp();
    const unsigned base = firstRealOperand(call);
    if (callee.getSymName() == "pthread_create")
        return base + 2 < call.getNumOperands() ? resolveFunctionOperand(traceToRoot(call.getOperand(base + 2)))
                                                : cir::FuncOp();
    if (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn) {
        const unsigned k = spawnEntryOperand(call);
        return k < call.getNumOperands() ? resolveFunctionOperand(traceToRoot(call.getOperand(k))) : cir::FuncOp();
    }
    return cir::FuncOp();
}

// A thread started from a real-time thread with the default attributes, or
// with attributes that do not ask for explicit scheduling, inherits its
// policy: what a real-time root reaches includes the threads it starts so.
// A std::thread it starts is one the analysis does not follow, and is
// reported as not checked.
void RequirementRoots::computeReach() {
    auto inherits = [&](cir::CallOp call) {
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return false;
        if (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn) return true;
        if (callee.getSymName() != "pthread_create") return false;
        const unsigned base = firstRealOperand(call);
        if (base + 1 >= call.getNumOperands()) return true;
        Value attr = call.getOperand(base + 1);
        if (constantOf(attr)) return true;  // null: the default attributes
        const Objects mine = objectsOf(attr);
        bool explicitSched = false;
        module.walk([&](cir::CallOp c) {
            cir::FuncOp cf = lookupCallee(c);
            if (!cf || cf.getSymName() != "pthread_attr_setinheritsched") return;
            const unsigned cb = firstRealOperand(c);
            // Only explicit scheduling on these very attributes lets the
            // thread not inherit: attributes that only may be these leave
            // it inheriting, and so checked.
            if (cb + 1 >= c.getNumOperands() || !mustBeSame(objectsOf(c.getOperand(cb)), mine)) return;
            auto k = constantOf(c.getOperand(cb + 1));
            if (k && *k == kExplicitSched) explicitSched = true;
        });
        return !explicitSched;
    };
    for (unsigned i = 0; i < roots_.size(); ++i) {
        const bool realtime = roots_[i].source == RequirementRoot::Source::realtime;
        std::deque<cir::FuncOp> work{roots_[i].func};
        if (!via_.try_emplace({roots_[i].func.getOperation(), i}, nullptr).second) continue;
        rootsOf_[roots_[i].func].push_back(i);
        while (!work.empty()) {
            cir::FuncOp f = work.front();
            work.pop_front();
            f.walk([&](cir::CallOp call) {
                llvm::SmallVector<cir::FuncOp, 4> targets;
                if (cir::FuncOp callee = lookupCallee(call)) targets.push_back(callee);
                else targets = indirectCallees(call);
                if (realtime && inherits(call)) {
                    // The started thread is itself one at real-time
                    // priority, checked as a root of its own.
                    if (cir::FuncOp e = spawnedEntry(call)) {
                        cir::FuncOp from = roots_[i].func;
                        const std::string how = roots_[i].why.substr(roots_[i].why.find('(') + 1);
                        addRealtime(e, call.getOperation(),
                                    "inherited from " + llvm::demangle(from.getSymName().str()) + ", " +
                                        how.substr(0, how.size() - 1));
                        targets.push_back(e);
                    }
                } else if (realtime) {
                    cir::FuncOp callee = lookupCallee(call);
                    const std::string name = callee ? llvm::demangle(callee.getSymName().str()) : std::string();
                    if (llvm::StringRef(name).starts_with("std::thread::thread"))
                        unresolved_.push_back({call, "starts, from a thread at real-time priority, a thread that "
                                                     "inherits its policy and that the analysis does not follow"});
                }
                for (cir::FuncOp t : targets) {
                    if (t.getBody().empty()) continue;
                    if (via_.try_emplace({t.getOperation(), i}, call.getOperation()).second) {
                        rootsOf_[t].push_back(i);
                        work.push_back(t);
                    }
                }
            });
        }
    }
}

llvm::SmallVector<const RequirementRoot *, 2> RequirementRoots::allReaching(cir::FuncOp f) const {
    llvm::SmallVector<const RequirementRoot *, 2> out;
    if (auto it = rootsOf_.find(f); it != rootsOf_.end())
        for (unsigned i : it->second) out.push_back(&roots_[i]);
    return out;
}

const RequirementRoot *RequirementRoots::reaching(cir::FuncOp f, llvm::SmallVectorImpl<cir::CallOp> *chain,
                                                  llvm::function_ref<bool(const RequirementRoot &)> which) const {
    auto it = rootsOf_.find(f);
    if (it == rootsOf_.end()) return nullptr;
    for (unsigned i : it->second) {
        if (which && !which(roots_[i])) continue;
        if (chain) {
            chain->clear();
            Operation *at = f;
            for (unsigned guard = 0; guard < 1024; ++guard) {
                auto v = via_.find({at, i});
                if (v == via_.end() || !v->second) break;
                chain->insert(chain->begin(), cast<cir::CallOp>(v->second));
                at = v->second->getParentOfType<cir::FuncOp>();
            }
        }
        return &roots_[i];
    }
    return nullptr;
}

} // namespace mlir::wtc
