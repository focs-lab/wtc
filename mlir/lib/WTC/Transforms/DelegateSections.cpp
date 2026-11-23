// --wtc-delegate-sections: the critical sections of one mutex, named by the
// build, run by a server thread for that mutex instead of under it by each
// thread (docs/delegation.md).
//
// A section is the code from a lock of the mutex to its unlock on every
// path: a lock object built on it (std::lock_guard, std::unique_lock, a
// class of the program's whose constructor locks the mutex it is handed
// and whose destructor unlocks it), a lock or unlock function of the
// program's that locks or unlocks a member of the object it is called on
// (LevelDB's port::Mutex), or pthread_mutex_lock and pthread_mutex_unlock.
// Wrappers are read from their bodies, not their names: a function whose
// only lock operation, through its callees, is one lock or one unlock of
// what a parameter points to is that operation on what the call hands it.
//
// The function that holds a section is first made a flat control-flow
// graph (cir-flatten-cfg) with its variables at its entry
// (cir-hoist-allocas): the sections of real code begin and end at
// different depths of its scopes (LevelDB's DBImpl::Get unlocks inside
// a nested block and locks again before a return that runs the lock
// object's destructor), so a section is a set of blocks, not a scope. The
// section is outlined into a function of its own that takes an
// environment, the addresses of the values it reads and of slots for the
// values the code after it reads, and returns the exit it left by; the
// mutex's own lock and unlock stay inside it, so that running it on the
// calling thread is the program as written. The call is a wtc.delegate,
// lowered by --lower-wtc-to-cir to the runtime (runtime/delegate.hpp).
//
// Deny by default: a section is delegated only when the pass proves, of
// it and of everything it calls,
//
//   (a) it cannot tell which thread runs it: no thread-local storage, no
//       errno, no thread id, signal state, affinity, thread name,
//       cancellation state, thread CPU clock or stack frame address, no
//       thread started (unless assume-uniform-threads says the program does
//       not depend on such a thread's attributes), no allocation by the C
//       library's malloc, which reports failure through errno, and, in the
//       section itself, no stack memory of run-time size;
//   (b) it is a section: one entry, the lock; every path from it reaches
//       an unlock of the mutex without leaving the function, taking the
//       mutex again or unwinding; no exception can leave it, and it does
//       not read its function's variadic arguments;
//   (c) every other lock it takes it lets go again within itself, and it
//       lets go none it did not take within itself before; no call through
//       a pointer may take or let go one; it takes no recursive mutex (a
//       pthread or C11 mutex only where the pass proves the program makes
//       none recursive); the named lock is not taken for reading; and it
//       reaches no other section of the same mutex;
//   (d) it waits for no other thread but by taking such a lock: no
//       condition wait, join, sleep, semaphore, input or output;
//   (e) the pass sees all of its code: no call of a function with no body
//       here that the audited list does not name, no indirect call it
//       cannot resolve, no inline assembly that does more than wait or
//       order memory.
//
// The remarks, one per section:
//
//   wtc: delegated section <f> #<n> on <mutex> delegated
//   wtc: delegated section <f> #<n> on <mutex> not delegated: (<c>) <why>
//
// The mutex is named by the option, `mutex=Class::member` for a member of
// a class (its name as CIR records it, `leveldb::DBImpl::mutex_`) or
// `mutex=name` for a global. Whether delegating it pays is the build's
// decision (docs/delegation.md, "When"): the pass has no cost model yet.

#include "WTC/Transforms/Blocking.h"
#include "WTC/Transforms/ClosedWorld.h"
#include "WTC/Transforms/CriticalSections.h"
#include "WTC/Transforms/LiftNames.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Refusal.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCDialect.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"
#include "clang/CIR/Dialect/Passes.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/RegionUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/Regex.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace mlir;
using namespace mlir::wtc;

namespace {

constexpr llvm::StringLiteral kIdiom = "delegated section";
constexpr llvm::StringLiteral kSectionAttr = "wtc.delegated_section";

// ---------------------------------------------------------------------------
// The mutex the option names.

struct MutexSpec {
    std::string text;            // as the option gave it
    std::string global;          // a global's symbol, or empty
    std::string record, member;  // a member of a class
};

std::optional<MutexSpec> parseSpec(StringRef text, ModuleOp module) {
    MutexSpec s;
    s.text = text.str();
    if (module.lookupSymbol<cir::GlobalOp>(text)) {
        s.global = text.str();
        return s;
    }
    size_t at = text.rfind("::");
    if (at == StringRef::npos || at == 0 || at + 2 >= text.size()) return std::nullopt;
    s.record = text.substr(0, at).str();
    s.member = text.substr(at + 2).str();
    return s;
}

// ---------------------------------------------------------------------------
// Lock operations, and the functions that wrap one.

// A lock operation: exclusive, shared (a read lock), or a release.
enum class Ev { Lock, Shared, Unlock };

bool takes(Ev e) { return e != Ev::Unlock; }

// Where a lock operation's mutex is, relative to a function: what a
// parameter points to, below some members (Param); what a member of
// what a parameter points to points to (ParamMember: a lock object's
// pointer to its mutex); or a place the summary cannot name (Unknown).
struct Target {
    enum Kind { Param, ParamMember, Global, Unknown } kind = Unknown;
    unsigned param = 0;
    std::string global;  // for Global
    llvm::SmallVector<unsigned, 4> path;
};

// Where an address is: inside what a parameter of f points to, or inside
// a global, with the members stepped through on the way (a step to a
// base class is no member step: the base is part of the object).
struct Place {
    bool isGlobal = false;
    unsigned param = 0;
    std::string global;
    llvm::SmallVector<unsigned, 4> path;
    bool operator==(const Place &o) const {
        return isGlobal == o.isGlobal && param == o.param && global == o.global && path == o.path;
    }
};

std::optional<Place> placeOf(Value v, cir::FuncOp f) {
    llvm::SmallVector<unsigned, 4> rev;
    for (unsigned guard = 0; v && guard < 64; ++guard) {
        v = traceToLocalRoot(v);
        if (auto gm = v.getDefiningOp<cir::GetMemberOp>()) {
            rev.push_back(gm.getIndex());
            v = gm.getAddr();
            continue;
        }
        if (auto b = v.getDefiningOp<cir::BaseClassAddrOp>()) {
            v = b.getDerivedAddr();
            continue;
        }
        if (auto d = v.getDefiningOp<cir::DerivedClassAddrOp>()) {
            v = d.getBaseAddr();
            continue;
        }
        if (Value r = traceToRoot(v); r != v) {
            v = r;
            continue;
        }
        Place p;
        p.path.assign(rev.rbegin(), rev.rend());
        if (auto a = dyn_cast<BlockArgument>(v)) {
            if (!f || a.getOwner() != &f.getBody().front()) return std::nullopt;
            p.param = a.getArgNumber();
            return p;
        }
        if (auto g = v.getDefiningOp<cir::GetGlobalOp>()) {
            p.isGlobal = true;
            p.global = g.getName().str();
            return p;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

struct Event {
    Ev ev;
    Target t;
};

// A store of a parameter into a member of what another parameter points
// to: a lock object's constructor keeping the mutex it locks.
struct Keep {
    unsigned dstParam;
    llvm::SmallVector<unsigned, 4> dstPath;
    unsigned srcParam;
};

struct Summary {
    llvm::SmallVector<Event, 2> events;  // the lock operations the function's callers see
    llvm::SmallVector<Keep, 1> keeps;
    int net = 0;                         // locks minus unlocks, over the code
};

std::optional<Ev> primitive(StringRef name) {
    static const llvm::StringSet<> locks = {"pthread_mutex_lock",      "pthread_mutex_trylock",
                                            "pthread_mutex_timedlock", "pthread_mutex_clocklock",
                                            "pthread_rwlock_wrlock",   "pthread_rwlock_trywrlock",
                                            "pthread_rwlock_timedwrlock", "pthread_rwlock_clockwrlock",
                                            "pthread_spin_lock",       "pthread_spin_trylock",
                                            "mtx_lock",                "mtx_trylock",
                                            "mtx_timedlock"};
    static const llvm::StringSet<> shared = {"pthread_rwlock_rdlock", "pthread_rwlock_tryrdlock",
                                             "pthread_rwlock_timedrdlock", "pthread_rwlock_clockrdlock"};
    static const llvm::StringSet<> unlocks = {"pthread_mutex_unlock", "pthread_rwlock_unlock",
                                              "pthread_spin_unlock", "mtx_unlock"};
    if (locks.contains(name)) return Ev::Lock;
    if (shared.contains(name)) return Ev::Shared;
    if (unlocks.contains(name)) return Ev::Unlock;
    return std::nullopt;
}

bool hasBody(cir::FuncOp f) {
    if (!f || f.getBody().empty()) return false;
    // A body the link may replace is no body (Blocking.h's rule).
    switch (f.getLinkage()) {
    case cir::GlobalLinkageKind::WeakAnyLinkage:
    case cir::GlobalLinkageKind::LinkOnceAnyLinkage:
    case cir::GlobalLinkageKind::ExternalWeakLinkage:
    case cir::GlobalLinkageKind::CommonLinkage:
        return false;
    default:
        return true;
    }
}

class LockSummaries {
public:
    const Summary &of(cir::FuncOp g) {
        auto it = memo.find(g.getOperation());
        if (it != memo.end()) return it->second;
        static const Summary empty;
        if (!hasBody(g) || !inProgress.insert(g.getOperation()).second) return empty;
        Summary s = compute(g);
        inProgress.erase(g.getOperation());
        return memo[g.getOperation()] = std::move(s);
    }

    // The lock operations a call makes, as its callee's summary or a
    // primitive says, with each target re-rooted at the caller.
    void eventsOf(cir::CallOp call, llvm::SmallVectorImpl<std::pair<Ev, Target>> &out, int &net) {
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return;
        cir::FuncOp f = call->getParentOfType<cir::FuncOp>();
        if (auto p = primitive(callee.getSymName())) {
            if (call.getArgOperands().empty()) return;
            out.push_back({*p, targetOf(call.getArgOperands()[0], f)});
            net += takes(*p) ? 1 : -1;
            return;
        }
        const Summary &s = of(callee);
        net += s.net;
        if (s.net == 0 && s.events.size() != 1) return;
        for (const Event &e : s.events) out.push_back({e.ev, mapThrough(e.t, call, f)});
    }

    // A target of the callee's, re-rooted at the caller through the call.
    Target mapThrough(const Target &t, cir::CallOp call, cir::FuncOp caller) {
        Target r;
        if (t.kind == Target::Global) return t;
        auto args = call.getArgOperands();
        if (t.kind == Target::Unknown || t.param >= args.size()) return r;
        Target base = targetOf(args[t.param], caller);
        if (t.kind == Target::Param) {
            if (base.kind == Target::Unknown) return r;
            // Below what a member points to, the steps name a part of the
            // same mutex object: the member is what identifies it.
            if (base.kind != Target::ParamMember) base.path.append(t.path.begin(), t.path.end());
            return base;
        }
        // ParamMember: what the member of the argument points to.
        if (base.kind != Target::Param) return r;
        r.kind = Target::ParamMember;
        r.param = base.param;
        r.path = base.path;
        r.path.append(t.path.begin(), t.path.end());
        return r;
    }

    // Where an address is, relative to f's parameters.
    static Target targetOf(Value addr, cir::FuncOp f) {
        Target t;
        if (!f) return t;
        if (auto p = placeOf(addr, f)) {
            t.kind = p->isGlobal ? Target::Global : Target::Param;
            t.param = p->param;
            t.global = p->global;
            t.path = p->path;
            return t;
        }
        // A pointer loaded from a member: the parameter stored there, if f
        // stores one (a constructor keeping its argument), else the member.
        auto load = traceToLocalRoot(addr).getDefiningOp<cir::LoadOp>();
        if (!load) return t;
        auto p = placeOf(load.getAddr(), f);
        if (!p || p->isGlobal) return t;
        std::optional<unsigned> stored;
        bool other = false;
        f.walk([&](cir::StoreOp st) {
            auto q = placeOf(st.getAddr(), f);
            if (!q || !(*q == *p)) return;
            auto v = placeOf(st.getValue(), f);
            if (v && !v->isGlobal && v->path.empty()) stored = v->param;
            else other = true;
        });
        if (stored && !other) {
            t.kind = Target::Param;
            t.param = *stored;
            return t;
        }
        t.kind = Target::ParamMember;
        t.param = p->param;
        t.path = p->path;
        return t;
    }

private:
    llvm::DenseMap<Operation *, Summary> memo;
    llvm::DenseSet<Operation *> inProgress;

    Summary compute(cir::FuncOp g) {
        Summary s;
        g.walk([&](Operation *op) {
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                llvm::SmallVector<std::pair<Ev, Target>, 2> evs;
                eventsOf(call, evs, s.net);
                for (auto &[ev, t] : evs) s.events.push_back(Event{ev, t});
                return;
            }
            if (auto st = dyn_cast<cir::StoreOp>(op)) {
                auto dst = placeOf(st.getAddr(), g);
                auto src = placeOf(st.getValue(), g);
                if (!dst || !src || dst->isGlobal || src->isGlobal || !src->path.empty() || dst->path.empty())
                    return;
                s.keeps.push_back(Keep{dst->param, dst->path, src->param});
            }
        });
        return s;
    }
};

// A name for the object an address is in: the value or global it is
// reached from, and the member steps (a step to a base is none).
std::string objectKey(Value v) {
    llvm::SmallVector<unsigned, 4> rev;
    for (unsigned guard = 0; v && guard < 64; ++guard) {
        v = traceToLocalRoot(v);
        if (auto gm = v.getDefiningOp<cir::GetMemberOp>()) {
            rev.push_back(gm.getIndex());
            v = gm.getAddr();
            continue;
        }
        if (auto b = v.getDefiningOp<cir::BaseClassAddrOp>()) {
            v = b.getDerivedAddr();
            continue;
        }
        if (Value r = traceToRoot(v); r != v) {
            v = r;
            continue;
        }
        break;
    }
    std::string k;
    if (auto g = v ? v.getDefiningOp<cir::GetGlobalOp>() : cir::GetGlobalOp())
        k = "@" + g.getName().str();
    else
        k = "%" + std::to_string(reinterpret_cast<uintptr_t>(v.getAsOpaquePointer()));
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) k += "." + std::to_string(*it);
    return k;
}

// Whether code lets go a lock it did not take within itself: an unlock
// with no lock of the same object before it in the code's order (the
// order of its blocks, or of its operations), as when a section drops a
// lock its caller holds and takes it back. The server would let go a
// lock another thread holds. The locks of a lock object are matched
// through the slot its constructor kept the mutex in; `exempt` are the
// section's own lock and unlocks. Counted over the code, not path by path.
std::optional<std::string> unlocksForeign(llvm::ArrayRef<Operation *> ops, const llvm::DenseSet<Operation *> &exempt,
                                          LockSummaries &locks) {
    std::map<std::pair<Operation *, std::vector<unsigned>>, std::string> kept;
    for (Operation *op : ops) {
        auto c = dyn_cast<cir::CallOp>(op);
        cir::FuncOp callee = c ? lookupCallee(c) : cir::FuncOp();
        if (!callee || !hasBody(callee)) continue;
        for (const Keep &k : locks.of(callee).keeps) {
            auto args = c.getArgOperands();
            if (k.dstParam >= args.size() || k.srcParam >= args.size()) continue;
            if (auto slot = traceToLocalRoot(args[k.dstParam]).getDefiningOp<cir::AllocaOp>())
                kept[{slot.getOperation(), std::vector<unsigned>(k.dstPath.begin(), k.dstPath.end())}] =
                    objectKey(args[k.srcParam]);
        }
    }
    std::map<std::string, int> held;
    for (Operation *op : ops) {
        auto c = dyn_cast<cir::CallOp>(op);
        if (!c || exempt.contains(op)) continue;
        cir::FuncOp callee = lookupCallee(c);
        if (!callee) continue;
        llvm::SmallVector<Event, 2> raw;
        if (auto p = primitive(callee.getSymName())) {
            Target t;
            t.kind = Target::Param;
            raw.push_back(Event{*p, t});
        } else if (hasBody(callee)) {
            const Summary &sum = locks.of(callee);
            if (sum.net != 0 || sum.events.size() == 1) raw.append(sum.events.begin(), sum.events.end());
        }
        auto args = c.getArgOperands();
        for (const Event &e : raw) {
            std::string key;
            if (e.t.kind == Target::Global) {
                key = "@" + e.t.global;
            } else if (e.t.kind == Target::Param && e.t.param < args.size()) {
                key = objectKey(args[e.t.param]);
                for (unsigned i : e.t.path) key += "." + std::to_string(i);
            } else if (e.t.kind == Target::ParamMember && e.t.param < args.size()) {
                if (auto slot = traceToLocalRoot(args[e.t.param]).getDefiningOp<cir::AllocaOp>()) {
                    auto it = kept.find({slot.getOperation(), std::vector<unsigned>(e.t.path.begin(), e.t.path.end())});
                    if (it != kept.end()) key = it->second;
                }
                if (key.empty()) {
                    key = objectKey(args[e.t.param]) + "*";
                    for (unsigned i : e.t.path) key += "." + std::to_string(i);
                }
            }
            if (takes(e.ev)) {
                if (!key.empty()) ++held[key];
                continue;
            }
            // An unlock of a lock taken within, or of a part of one (a
            // wrapper's member mutex).
            bool found = false;
            for (auto &[k, n] : held)
                if (n > 0 && !key.empty() && (key == k || StringRef(key).starts_with(k + ".") ||
                                              StringRef(k).starts_with(key + "."))) {
                    --n;
                    found = true;
                    break;
                }
            if (!found)
                return "(c) lets go a lock it did not take within the section (" + shortName(callee) +
                       "): the server would let go a lock another thread holds";
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// The audited list: functions with no body here that a delegated section
// may call, and what every other one is.

enum class Allowed { Yes, Lock, StartsThread };

std::optional<Allowed> allowedExternal(cir::FuncOp f) {
    const StringRef n = f.getSymName();
    if (primitive(n)) return Allowed::Lock;
    static const llvm::StringSet<> inert = {
        // memory and strings handed to them, nothing else, and no errno
        // (touchesOnlyItsArguments' list less sqrt and sqrtf, which set
        // EDOM)
        "memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strcmp", "strncmp", "abs", "labs",
        "llabs", "fabs", "fabsf",
        // freeing, and the global operator new and delete
        "free",
        // waking a waiter: not a wait, and nothing of the caller's thread
        "pthread_cond_signal", "pthread_cond_broadcast", "_ZNSt18condition_variable10notify_oneEv",
        "_ZNSt18condition_variable10notify_allEv",
        // a thread's handle let go; the base of std::thread's state, whose
        // destructor libstdc++ defines as the default, destroying nothing
        "pthread_detach", "_ZNSt6thread6detachEv", "_ZNSt6thread6_StateD2Ev", "_ZNSt6thread6_StateD1Ev",
        "_ZNSt6thread6_StateD0Ev",
        // the string and memory functions not in touchesOnlyItsArguments
        "strnlen", "strchr", "strrchr", "memrchr", "strstr", "strcpy", "strncpy", "strcat", "strncat",
        // clocks of the system (clock_gettime is read at its call, for
        // its clock: a thread's CPU clock is the server's)
        "gettimeofday", "time",
        // what ends the program, or throws: no exception can leave a
        // delegated section ((b)), so one thrown inside ends the program
        // on the server as it would have on the client
        "abort", "__assert_fail", "__stack_chk_fail", "__cxa_pure_virtual", "_ZSt9terminatev",
        "__cxa_throw", "__cxa_allocate_exception", "__cxa_free_exception", "__cxa_begin_catch",
        "__cxa_end_catch", "__cxa_rethrow", "_Unwind_Resume"};
    if (inert.contains(n)) return Allowed::Yes;
    // The global operator new and delete, but not the nothrow new, whose
    // failure leaves errno set (by malloc) and returns null.
    static const llvm::Regex newDelete("^_Z(n[wa][mj](St11align_val_t)?"
                                       "|d[la]Pv[mj]?(St11align_val_t)?(RKSt9nothrow_t)?)$");
    if (newDelete.match(n)) return Allowed::Yes;
    // libstdc++'s __throw_* helpers.
    if (StringRef(llvm::demangle(n.str())).starts_with("std::__throw_")) return Allowed::Yes;
    if (n == "pthread_create" || n.starts_with("_ZNSt6thread15_M_start_thread")) return Allowed::StartsThread;
    return std::nullopt;
}

// A function the section may not call because of the thread it runs on,
// and why.
std::optional<std::string> identityExternal(cir::FuncOp f) {
    static const llvm::StringSet<> names = {
        "pthread_self",          "gettid",                 "syscall",
        "__errno_location",      "pthread_getspecific",    "pthread_setspecific",
        "pthread_key_create",    "pthread_key_delete",     "pthread_sigmask",
        "sigprocmask",           "sigaction",              "signal",
        "raise",                 "pthread_kill",           "sched_getcpu",
        "getcpu",                "pthread_getaffinity_np", "pthread_setaffinity_np",
        "sched_setaffinity",     "sched_getaffinity",      "pthread_setname_np",
        "pthread_getname_np",    "prctl",                  "pthread_getattr_np",
        "pthread_exit",          "pthread_setcancelstate", "pthread_setcanceltype",
        "pthread_testcancel",    "setjmp",                 "_setjmp",
        "sigsetjmp",             "__sigsetjmp",            "longjmp",
        "siglongjmp",            "getrusage",              "pthread_getcpuclockid",
        "sched_setscheduler",    "pthread_setschedparam",  "pthread_getschedparam",
        "setpriority",           "nice"};
    const StringRef n = f.getSymName();
    if (names.contains(n)) return "depends on the thread that runs it: it calls " + llvm::demangle(n.str());
    static const llvm::StringSet<> errnoAllocators = {"malloc", "calloc", "realloc", "aligned_alloc",
                                                      "posix_memalign", "memalign", "strdup", "strndup",
                                                      "valloc", "pvalloc"};
    if (errnoAllocators.contains(n))
        return "depends on the thread that runs it: " + n.str() +
               " reports a failure through errno, which is the server's";
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// The legality of the code a section runs.

class CodeCheck {
public:
    CodeCheck(ModuleOp module, LockSummaries &locks, bool uniformThreads, bool resolveVirtual)
        : module(module), locks(locks), uniformThreads(uniformThreads), resolveVirtual(resolveVirtual) {
        allocatorHook = allocatorCallbackRegistration(module);
        module.walk([&](Operation *op) {
            auto note = [&](Type t) {
                t.walk([&](cir::RecordType r) {
                    if (r.getName()) records.try_emplace(squashSpaces(r.getName().getValue()), r);
                });
            };
            for (Type t : op->getResultTypes()) note(t);
            if (auto g = dyn_cast<cir::GlobalOp>(op)) note(g.getSymType());
            if (auto f = dyn_cast<cir::FuncOp>(op)) note(f.getFunctionType());
        });
        module.walk([&](cir::GlobalOp g) {
            StringRef sym = g.getSymName();
            if (!sym.starts_with("_ZTV")) return;
            VTable vt;
            std::string d = llvm::demangle(sym.str());
            if (StringRef(d).starts_with("vtable for ")) vt.cls = squashSpaces(StringRef(d).drop_front(11));
            if (auto init = dyn_cast_or_null<cir::VTableAttr>(g.getInitialValue().value_or(Attribute())))
                vt.data = init.getData();
            else if (!g.isDeclaration())
                return;
            vtables.push_back(vt);
        });
    }

    // The first reason an operation of a section, or anything it calls,
    // may not run on the server.
    std::optional<std::string> at(Operation *op) {
        std::optional<std::string> found;
        op->walk([&](Operation *o) {
            if (auto r = local(o)) {
                found = r;
                return WalkResult::interrupt();
            }
            return WalkResult::advance();
        });
        return found;
    }

    // Whether the code reaches a pthread or C11 lock primitive called by
    // the program's own code, not through libstdc++'s std::mutex and its
    // kind, whose mutexes are never recursive.
    bool reachesRawLock(llvm::ArrayRef<Operation *> ops) {
        llvm::DenseSet<Operation *> seen;
        std::function<bool(Operation *)> walk = [&](Operation *root) {
            bool raw = false;
            root->walk([&](cir::CallOp c) {
                llvm::SmallVector<cir::FuncOp, 4> targets;
                if (c.isIndirect()) {
                    targets = indirectCallees(c);
                    if (targets.empty()) targets = virtualTargets(c);
                } else if (cir::FuncOp t = lookupCallee(c)) {
                    targets.push_back(t);
                }
                for (cir::FuncOp t : targets) {
                    if (primitive(t.getSymName())) {
                        auto in = c->getParentOfType<cir::FuncOp>();
                        StringRef caller = in ? in.getSymName() : StringRef();
                        if (!caller.contains("__gthread_") && !caller.contains("__glibcxx_rwlock")) raw = true;
                    } else if (hasBody(t) && seen.insert(t.getOperation()).second && walk(t)) {
                        raw = true;
                    }
                    if (raw) return WalkResult::interrupt();
                }
                return WalkResult::advance();
            });
            return raw;
        };
        for (Operation *op : ops)
            if (walk(op)) return true;
        return false;
    }

private:
    ModuleOp module;
    LockSummaries &locks;
    bool uniformThreads, resolveVirtual;
    std::optional<std::string> allocatorHook;
    struct VTable {
        std::string cls;  // the class, demangled and squashed; empty when unknown
        ArrayAttr data;   // its groups; null for a vtable only declared here
    };
    std::vector<VTable> vtables;
    llvm::StringMap<cir::RecordType> records;
    std::map<std::string, bool> holdsMemo;
    llvm::DenseMap<Operation *, std::optional<std::string>> memo;
    llvm::DenseSet<Operation *> inProgress;
    bool metCycle = false;

    std::optional<std::string> function(cir::FuncOp g) {
        auto it = memo.find(g.getOperation());
        if (it != memo.end()) return it->second;
        if (!inProgress.insert(g.getOperation()).second) {
            metCycle = true;
            return std::nullopt;
        }
        bool outerCycle = metCycle;
        metCycle = false;
        std::optional<std::string> found;
        // Each function at most once per question: a callee that is
        // recursive, or recursive with its callers, is walked once.
        StringRef name = g.getSymName();
        std::string pretty = llvm::demangle(name.str());
        if (StringRef(pretty).contains("recursive_mutex") || name.contains("__gthread_recursive_mutex"))
            found = "(c) takes a recursive mutex (" + pretty + "): the server is not the thread that may hold it";
        // Its locks: no lock it lets go that it did not take (a wrapper,
        // one lock or one unlock, is checked where it is called).
        if (!found) {
            const Summary &own = locks.of(g);
            if (!(own.events.size() == 1 && own.net != 0)) {
                llvm::SmallVector<Operation *> body;
                g.walk([&](Operation *o) { body.push_back(o); });
                found = unlocksForeign(body, {}, locks);
            }
        }
        if (!found)
            g.walk([&](Operation *o) {
                if (auto r = local(o)) {
                    // The chain of calls, outermost first.
                    size_t at = r->find(" (through ");
                    std::string here = nameOf(g.getSymName()).ok ? shortName(g) : llvm::demangle(g.getSymName().str());
                    if (at == std::string::npos) found = *r + " (through " + here + ")";
                    else found = r->substr(0, at) + " (through " + here + ", " + r->substr(at + 10);
                    return WalkResult::interrupt();
                }
                return WalkResult::advance();
            });
        inProgress.erase(g.getOperation());
        if (!metCycle || found) memo[g.getOperation()] = found;
        metCycle = outerCycle || metCycle;
        return found;
    }

    std::optional<std::string> local(Operation *o) {
        if (auto gg = dyn_cast<cir::GetGlobalOp>(o)) {
            auto g = module.lookupSymbol<cir::GlobalOp>(gg.getName());
            if (gg.getTls() || (g && g.getTlsModel()))
                return "(a) depends on the thread that runs it: it reads the thread-local " +
                       llvm::demangle(gg.getName().str());
            return std::nullopt;
        }
        if (auto asmOp = dyn_cast<cir::InlineAsmOp>(o)) {
            StringRef text = asmOp.getAsmString();
            if (onlyNonBlockingInstructions(text) && !text.contains("rdtscp") && !text.contains("rdpid") &&
                !text.contains("cpuid"))
                return std::nullopt;
            return "(e) runs inline assembly the pass does not read";
        }
        if (isa<cir::IndirectBrOp>(o)) return std::nullopt;
        if (isa<cir::FrameAddrOp, cir::AddrOfReturnAddrOp>(o))
            return "(a) depends on the thread that runs it: it reads the address of a stack frame";
        if (auto in = dyn_cast<cir::LLVMIntrinsicCallOp>(o)) {
            // Deny by default: the intrinsics that only wait or order memory.
            static const llvm::StringSet<> inert = {"x86.sse2.pause", "x86.sse2.mfence", "x86.sse2.lfence",
                                                    "x86.sse.sfence", "prefetch", "expect", "assume"};
            if (inert.contains(in.getIntrinsicName())) return std::nullopt;
            return "(e) calls the intrinsic " + in.getIntrinsicName().str() + ", which is not on the audited list";
        }
        auto call = dyn_cast<cir::CallOp>(o);
        if (!call) return std::nullopt;
        if (call.isIndirect()) return indirect(call);
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return "(e) calls a function the pass cannot find";
        if (callee.getSymName() == "clock_gettime" && !hasBody(callee)) {
            // The system's clocks; not a thread's CPU clock, nor one the
            // pass cannot read (an unknown id also sets errno).
            std::optional<int64_t> id;
            if (!call.getArgOperands().empty())
                if (auto k = traceToRoot(call.getArgOperands()[0]).getDefiningOp<cir::ConstantOp>())
                    if (auto i = dyn_cast<cir::IntAttr>(k.getValue())) id = i.getSInt();
            // CLOCK_REALTIME, _MONOTONIC, _PROCESS_CPUTIME_ID, _MONOTONIC_RAW,
            // _REALTIME_COARSE, _MONOTONIC_COARSE, _BOOTTIME, _TAI.
            if (!id || !llvm::is_contained({0, 1, 2, 4, 5, 6, 7, 11}, *id))
                return "(a) depends on the thread that runs it: it reads a clock that may be the thread's own "
                       "(clock_gettime with " + (id ? "clock " + std::to_string(*id) : std::string("a clock read at run time")) + ")";
            return std::nullopt;
        }
        return callOf(callee);
    }

    std::optional<std::string> callOf(cir::FuncOp callee) {
        if (hasBody(callee)) return function(callee);
        if (auto why = identityExternal(callee)) return "(a) " + *why;
        if (auto a = allowedExternal(callee)) {
            if (*a == Allowed::StartsThread && !uniformThreads)
                return "(a) starts a thread (" + llvm::demangle(callee.getSymName().str()) +
                       "), which inherits the signal mask, affinity and scheduling of the thread that runs "
                       "it (assume-uniform-threads admits it)";
            if (allocatorHook && callee.getSymName().starts_with("_Z") && !primitive(callee.getSymName()))
                if (StringRef(callee.getSymName()).starts_with("_Zn") || StringRef(callee.getSymName()).starts_with("_Zd"))
                    return "(e) allocates, and the program registers code the allocator runs (" + *allocatorHook +
                           ")";
            if (*a == Allowed::Lock) {
                StringRef n = callee.getSymName();
                if (n.contains("timedlock") || n.contains("clocklock"))
                    return "(d) waits for a lock with a time limit (" + n.str() + ")";
            }
            return std::nullopt;
        }
        if (auto ext = externalBlocking(callee)) {
            switch (ext->kind) {
            case BlockingKind::Waits:
            case BlockingKind::Sleeps:
            case BlockingKind::InputOutput:
                return "(d) " + ext->what;
            case BlockingKind::Threading:
                return "(a) " + ext->what;
            default:
                break;
            }
        }
        return "(e) calls " + llvm::demangle(callee.getSymName().str()) +
               ", which has no body here and is not on the audited list";
    }

    // An indirect call: the functions a function pointer parameter may be
    // handed (indirectCallees), or a virtual call's slot in every vtable of
    // the module, of the call's signature, when the module is the whole
    // program.
    std::optional<std::string> indirect(cir::CallOp call) {
        auto targets = indirectCallees(call);
        if (targets.empty()) targets = virtualTargets(call);
        if (targets.empty()) {
            if (auto self = virtualReceiver(call))
                return "(e) makes a virtual call on " + *self + " the pass cannot resolve";
            return "(e) makes an indirect call the pass cannot resolve";
        }
        for (cir::FuncOp t : targets) {
            // A lock taken or let go through the pointer: the caller's
            // balance cannot see it.
            if (locks.of(t).net != 0)
                return "(c) a call through a pointer may take or let go a lock (" + shortName(t) + ")";
            if (auto r = callOf(t)) return r;
        }
        return std::nullopt;
    }

    // A virtual call through a pointer to class T may reach the function
    // in its slot of the vtable of T or of any class derived from T. The
    // module's records do not say which members are bases, so a class is
    // taken to derive from T when it holds T, as a base or as a member, at
    // any depth: more classes than derive from it, never fewer. A vtable
    // whose class the pass cannot find among the module's records is
    // taken to be one of them. A vtable the module only declares (a class
    // of the system's libraries) of T or of a class holding T leaves the
    // call unresolved, except the audited abstract classes below, which no
    // object has as its own type.
    // The class a virtual call is made on, for remarks.
    std::optional<std::string> virtualReceiver(cir::CallOp call) {
        if (call->getNumOperands() == 0 || call.getArgOperands().empty()) return std::nullopt;
        auto load = traceToLocalRoot(call->getOperand(0)).getDefiningOp<cir::LoadOp>();
        if (!load || !load.getAddr().getDefiningOp<cir::VTableGetVirtualFnAddrOp>()) return std::nullopt;
        auto self = dyn_cast<cir::PointerType>(call.getArgOperands()[0].getType());
        StringRef n = self ? recordName(self.getPointee()) : StringRef();
        return n.empty() ? std::string("an object") : n.str();
    }

    llvm::SmallVector<cir::FuncOp, 4> virtualTargets(cir::CallOp call) {
        llvm::SmallVector<cir::FuncOp, 4> out;
        if (!resolveVirtual || call->getNumOperands() == 0) return out;
        auto load = traceToLocalRoot(call->getOperand(0)).getDefiningOp<cir::LoadOp>();
        if (!load) return out;
        auto slot = load.getAddr().getDefiningOp<cir::VTableGetVirtualFnAddrOp>();
        if (!slot) return out;
        auto args = call.getArgOperands();
        if (args.empty()) return out;
        auto self = dyn_cast<cir::PointerType>(args[0].getType());
        auto cls = self ? dyn_cast<cir::RecordType>(self.getPointee()) : cir::RecordType();
        if (!cls || !cls.getName()) return out;
        const std::string T = squashSpaces(cls.getName().getValue());
        // The classes of the system's libraries that are abstract: an
        // object of theirs is always of a derived class, whose vtable a
        // program that makes it has. std::thread::_State declares _M_run
        // pure; std::thread's constructor makes a _State_impl.
        static const llvm::StringSet<> abstractLibrary = {"std::thread::_State"};
        const uint64_t index = 2 + slot.getIndex();
        llvm::SetVector<Operation *> seen;
        for (const VTable &vt : vtables) {
            if (!vt.data) {
                // A class of the system's libraries derives from no class
                // of the program's: only one whose record the module has
                // can hold T.
                if (vt.cls.empty() || !records.count(vt.cls) || (vt.cls != T && !holds(vt.cls, T))) continue;
                if (abstractLibrary.contains(vt.cls)) continue;
                return {};
            }
            if (!mayDerive(vt, T)) continue;
            for (Attribute group : vt.data) {
                auto arr = dyn_cast<cir::ConstArrayAttr>(group);
                if (!arr) continue;
                auto elts = dyn_cast<ArrayAttr>(arr.getElts());
                if (!elts || elts.size() <= index) continue;
                auto view = dyn_cast<cir::GlobalViewAttr>(elts[index]);
                if (!view) continue;
                auto f = module.lookupSymbol<cir::FuncOp>(view.getSymbol().getValue());
                if (!f) continue;
                // The call's signature, the object aside.
                auto ft = f.getFunctionType();
                if (ft.getNumInputs() != args.size()) continue;
                bool same = true;
                for (unsigned i = 1; i < args.size(); ++i) same &= ft.getInput(i) == args[i].getType();
                if (same && seen.insert(f.getOperation())) out.push_back(f);
            }
        }
        return out;
    }

    // Whether a vtable's class may be T or derive from T: its record, found
    // by the vtable's name or as the object of a function in it, is T or
    // holds T. A class that overrides nothing has in the slot what T's own
    // vtable has there, so the functions' objects stand for it; a vtable
    // the pass can say nothing of is taken to be one.
    bool mayDerive(const VTable &vt, const std::string &T) {
        bool known = false;
        if (!vt.cls.empty() && records.count(vt.cls)) {
            known = true;
            if (vt.cls == T || holds(vt.cls, T)) return true;
        }
        for (Attribute group : vt.data) {
            auto arr = dyn_cast<cir::ConstArrayAttr>(group);
            auto elts = arr ? dyn_cast<ArrayAttr>(arr.getElts()) : ArrayAttr();
            if (!elts) continue;
            for (Attribute e : elts) {
                auto view = dyn_cast<cir::GlobalViewAttr>(e);
                auto f = view ? module.lookupSymbol<cir::FuncOp>(view.getSymbol().getValue()) : cir::FuncOp();
                if (!f || f.getFunctionType().getNumInputs() == 0) continue;
                auto ptr = dyn_cast<cir::PointerType>(f.getFunctionType().getInput(0));
                auto r = ptr ? dyn_cast<cir::RecordType>(ptr.getPointee()) : cir::RecordType();
                if (!r || !r.getName()) continue;
                known = true;
                std::string n = squashSpaces(r.getName().getValue());
                if (n == T || holdsRec(r, T)) return true;
            }
        }
        return !known;
    }

    // Whether the record named `outer` holds the record named `inner`, at
    // any depth (squashed names); true when `outer` is not among the
    // module's records, which the pass cannot read.
    bool holds(const std::string &outer, const std::string &inner) {
        auto rec = records.find(outer);
        if (rec == records.end()) return true;
        return holdsRec(rec->second, inner);
    }
    bool holdsRec(cir::RecordType r, const std::string &inner) {
        std::string key = squashSpaces(r.getName() ? r.getName().getValue() : "") + "\n" + inner;
        auto it = holdsMemo.find(key);
        if (it != holdsMemo.end()) return it->second;
        holdsMemo[key] = false;  // a record that holds itself through a cycle
        bool found = false;
        for (Type m : r.getMembers()) {
            while (auto arr = dyn_cast<cir::ArrayType>(m)) m = arr.getElementType();
            auto mr = dyn_cast<cir::RecordType>(m);
            if (!mr) continue;
            if ((mr.getName() && squashSpaces(mr.getName().getValue()) == inner) || holdsRec(mr, inner)) {
                found = true;
                break;
            }
        }
        return holdsMemo[key] = found;
    }
};

// ---------------------------------------------------------------------------
// The pass.

bool sameMutex(Value a, Value b);

class DelegateSectionsPass : public PassWrapper<DelegateSectionsPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(DelegateSectionsPass)

    DelegateSectionsPass() = default;
    DelegateSectionsPass(const DelegateSectionsPass &other) : PassWrapper(other) {}

    StringRef getArgument() const final { return "wtc-delegate-sections"; }
    StringRef getDescription() const final {
        return "Run the critical sections of the named mutex on a delegation server thread, where the pass "
               "proves they cannot tell (docs/delegation.md); run last before --lower-wtc-to-cir";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    Option<std::string> mutexOpt{*this, "mutex",
                                 llvm::cl::desc("The mutex whose sections to delegate: Class::member, or a "
                                                "global's name; several separated by commas")};
    Option<bool> rewrite{*this, "rewrite", llvm::cl::init(true),
                         llvm::cl::desc("Outline and delegate the sections that pass (false: remarks only)")};
    Option<bool> uniformThreads{
        *this, "assume-uniform-threads", llvm::cl::init(false),
        llvm::cl::desc("Admit a section that starts a thread: the clients share their signal mask, affinity "
                       "and scheduling attributes, which the server and its children inherit")};
    Option<bool> openWorld{*this, "open-world", llvm::cl::init(false),
                           llvm::cl::desc("The module is linked later with files the pass does not see")};
    Option<std::string> namedElsewhere{
        *this, "named-elsewhere", llvm::cl::init(""),
        llvm::cl::desc("A file listing the symbols the files linked with the module define or reference")};
    Option<std::string> definedElsewhere{
        *this, "defined-elsewhere", llvm::cl::init(""),
        llvm::cl::desc("A file listing the symbols the files linked with the module define")};

    void runOnOperation() override;

private:
    ModuleOp module;
    unsigned outlined = 0;

    struct LockSite {
        Operation *op;
        Ev ev;
        Value key;           // the mutex's address, for a lock or unlock of it
        std::string global;  // or the global it is, taken through a wrapper
        bool same(const LockSite &o) const {
            if (!global.empty() || !o.global.empty()) return global == o.global;
            return sameMutex(key, o.key);
        }
    };

    std::optional<Value> mutexAddr(Value v, const MutexSpec &spec);
    llvm::SmallVector<LockSite> lockSites(cir::FuncOp f, const MutexSpec &spec, LockSummaries &locks);
    std::string recursionRisk(bool wholeProgram);
    std::string recursiveWhy;
    void handle(cir::FuncOp f, const MutexSpec &spec, LockSummaries &locks, CodeCheck &check);
};

std::optional<Value> DelegateSectionsPass::mutexAddr(Value v, const MutexSpec &spec) {
    for (unsigned guard = 0; v && guard < 64; ++guard) {
        v = traceToLocalRoot(v);
        if (auto gm = v.getDefiningOp<cir::GetMemberOp>()) {
            auto base = dyn_cast<cir::PointerType>(gm.getAddr().getType());
            if (spec.global.empty() && gm.getName() == spec.member && base && recordName(base.getPointee()) == spec.record)
                return v;
            v = gm.getAddr();
            continue;
        }
        if (auto gg = v.getDefiningOp<cir::GetGlobalOp>())
            return !spec.global.empty() && gg.getName() == spec.global ? std::optional<Value>(v) : std::nullopt;
        if (auto b = v.getDefiningOp<cir::BaseClassAddrOp>()) {
            v = b.getDerivedAddr();
            continue;
        }
        if (Value r = traceToRoot(v); r != v) {
            v = r;
            continue;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

// Whether two addresses of the named mutex are the same object: the same
// member path from the same value (a parameter, a global, a pointer kept
// in a variable written once).
bool sameMutex(Value a, Value b) {
    auto rooted = [](Value v, llvm::SmallVectorImpl<unsigned> &rev) -> Value {
        for (unsigned guard = 0; v && guard < 64; ++guard) {
            v = traceToLocalRoot(v);
            if (auto gm = v.getDefiningOp<cir::GetMemberOp>()) {
                rev.push_back(gm.getIndex());
                v = gm.getAddr();
                continue;
            }
            if (auto bc = v.getDefiningOp<cir::BaseClassAddrOp>()) {
                v = bc.getDerivedAddr();
                continue;
            }
            if (Value r = traceToRoot(v); r != v) {
                v = r;
                continue;
            }
            if (auto g = v.getDefiningOp<cir::GetGlobalOp>()) return Value();  // compared by name below
            return v;
        }
        return Value();
    };
    if (a == b) return true;
    llvm::SmallVector<unsigned, 4> pa, pb;
    Value ra = rooted(a, pa), rb = rooted(b, pb);
    if (ra && rb) return ra == rb && pa == pb;
    auto ga = memberPathOf(a), gb = memberPathOf(b);
    return ga && gb && *ga == *gb;
}

// The locks and unlocks of the mutex in f, in the order of f's blocks. A
// lock object's unlock is found through the slot its constructor kept the
// mutex in.
llvm::SmallVector<DelegateSectionsPass::LockSite> DelegateSectionsPass::lockSites(cir::FuncOp f, const MutexSpec &spec,
                                                                                LockSummaries &locks) {
    llvm::SmallVector<LockSite> out;
    // (slot, member path) -> the mutex's address kept there.
    std::map<std::pair<Operation *, std::vector<unsigned>>, Value> kept;
    llvm::SmallVector<cir::CallOp> calls;
    f.walk([&](cir::CallOp c) { calls.push_back(c); });
    // Keeps first, so that a destructor met before its constructor in the
    // walk still finds the slot.
    for (cir::CallOp c : calls) {
        cir::FuncOp callee = lookupCallee(c);
        if (!callee || !hasBody(callee)) continue;
        for (const Keep &k : locks.of(callee).keeps) {
            auto args = c.getArgOperands();
            if (k.dstParam >= args.size() || k.srcParam >= args.size()) continue;
            auto slot = traceToLocalRoot(args[k.dstParam]).getDefiningOp<cir::AllocaOp>();
            auto m = mutexAddr(args[k.srcParam], spec);
            if (slot && m) kept[{slot.getOperation(), std::vector<unsigned>(k.dstPath.begin(), k.dstPath.end())}] = *m;
        }
    }
    for (cir::CallOp c : calls) {
        cir::FuncOp callee = lookupCallee(c);
        if (!callee) continue;
        // The events as the callee's summary states them, on the call's
        // own operands.
        llvm::SmallVector<Event, 2> raw;
        if (auto p = primitive(callee.getSymName())) {
            Target t;
            t.kind = Target::Param;
            t.param = 0;
            raw.push_back(Event{*p, t});
        } else if (hasBody(callee)) {
            const Summary &s = locks.of(callee);
            if (s.net != 0 || s.events.size() == 1) raw.append(s.events.begin(), s.events.end());
        }
        auto args = c.getArgOperands();
        for (const Event &e : raw) {
            if (e.t.kind == Target::Global) {
                // A wrapper of a global mutex: its address, taken here.
                if (spec.global != e.t.global || !module.lookupSymbol<cir::GlobalOp>(e.t.global)) continue;
                out.push_back(LockSite{c, e.ev, Value(), e.t.global});
                continue;
            }
            if (e.t.kind == Target::Unknown || e.t.param >= args.size()) continue;
            std::optional<Value> key;
            if (e.t.kind == Target::Param) {
                key = mutexAddr(args[e.t.param], spec);
            } else if (auto slot = traceToLocalRoot(args[e.t.param]).getDefiningOp<cir::AllocaOp>()) {
                auto it = kept.find({slot.getOperation(), std::vector<unsigned>(e.t.path.begin(), e.t.path.end())});
                if (it != kept.end()) key = it->second;
            }
            if (key) out.push_back(LockSite{c, e.ev, *key, ""});
        }
    }
    return out;
}

// No pthread mutex of the module may be recursive: the server would wait
// for a lock its client holds, where the client itself would have taken
// it again. The module makes one only by setting a mutex attribute's type
// or by a static initialiser that is not all zeros.
// Whether a constant is zero all through: PTHREAD_MUTEX_INITIALIZER is,
// PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP is not.
bool allZero(Attribute a) {
    if (isa<cir::ZeroAttr>(a)) return true;
    if (auto i = dyn_cast<cir::IntAttr>(a)) return i.getValue().isZero();
    if (auto b = dyn_cast<cir::BoolAttr>(a)) return !b.getValue();
    if (auto p = dyn_cast<cir::ConstPtrAttr>(a)) return p.isNullValue();
    if (auto r = dyn_cast<cir::ConstRecordAttr>(a)) return llvm::all_of(r.getMembers(), allZero);
    if (auto arr = dyn_cast<cir::ConstArrayAttr>(a)) {
        if (auto elts = dyn_cast<ArrayAttr>(arr.getElts())) return llvm::all_of(elts, allZero);
        return false;
    }
    if (auto arr = dyn_cast<ArrayAttr>(a)) return llvm::all_of(arr, allZero);
    return false;
}

// Whether a constant holds, at any depth, a pthread mutex other than all
// zeros: PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP and its kind, in a global's
// initialiser, a member of one, or a constant a constructor stores (a C++
// default member initialiser).
bool holdsNonzeroPthreadMutex(Attribute a) {
    if (auto t = dyn_cast<TypedAttr>(a)) {
        StringRef n = recordName(t.getType());
        if ((n == "pthread_mutex_t" || n.ends_with("pthread_mutex_t")) && !allZero(a)) return true;
    }
    if (auto r = dyn_cast<cir::ConstRecordAttr>(a)) return llvm::any_of(r.getMembers(), holdsNonzeroPthreadMutex);
    if (auto arr = dyn_cast<cir::ConstArrayAttr>(a))
        if (auto elts = dyn_cast<ArrayAttr>(arr.getElts())) return llvm::any_of(elts, holdsNonzeroPthreadMutex);
    if (auto arr = dyn_cast<ArrayAttr>(a)) return llvm::any_of(arr, holdsNonzeroPthreadMutex);
    return false;
}

// Why a pthread or C11 mutex of the program may be recursive, or nothing
// when the module proves none is. Deny by default: a mutex is made
// recursive by a mutex attribute's type (pthread_mutexattr_settype), by a
// static initialiser that is not all zeros, or by mtx_init with
// mtx_recursive; any of them in the module, or files the pass does not
// see, leave it unproved. std::mutex and its kind never are.
std::string DelegateSectionsPass::recursionRisk(bool wholeProgram) {
    std::string why;
    module.walk([&](Operation *op) {
        if (auto c = dyn_cast<cir::CallOp>(op)) {
            cir::FuncOp callee = lookupCallee(c);
            StringRef n = callee ? callee.getSymName() : StringRef();
            if (n == "pthread_mutexattr_settype")
                why = "the module sets a mutex attribute's type (pthread_mutexattr_settype)";
            if (n == "mtx_init") {
                std::optional<int64_t> type;
                if (c.getArgOperands().size() > 1)
                    if (auto k = traceToRoot(c.getArgOperands()[1]).getDefiningOp<cir::ConstantOp>())
                        if (auto i = dyn_cast<cir::IntAttr>(k.getValue())) type = i.getSInt();
                if (!type || (*type & 1))  // mtx_recursive
                    why = "the module makes a C11 mutex that may be recursive (mtx_init)";
            }
        } else if (auto k = dyn_cast<cir::ConstantOp>(op)) {
            if (holdsNonzeroPthreadMutex(k.getValue()))
                why = "the module builds a pthread mutex from a constant other than zeros (a recursive initialiser)";
        } else if (auto g = dyn_cast<cir::GlobalOp>(op)) {
            if (auto init = g.getInitialValue(); init && holdsNonzeroPthreadMutex(*init))
                why = "the global " + g.getSymName().str() +
                      " holds a pthread mutex initialised to other than zeros (a recursive initialiser)";
        }
    });
    if (why.empty() && !wholeProgram)
        why = "files the pass does not see may make a pthread or C11 mutex recursive";
    return why;
}

void DelegateSectionsPass::runOnOperation() {
    module = getOperation();
    if (mutexOpt.empty()) {
        module.emitError() << "wtc: --wtc-delegate-sections needs mutex=Class::member or mutex=global";
        return signalPassFailure();
    }
    ClosedWorld world;
    ClosedWorldOptions o;
    o.openWorld = openWorld;
    o.namedElsewhereFile = namedElsewhere;
    o.definedElsewhereFile = definedElsewhere;
    if (failed(world.init(module, o))) return signalPassFailure();
    // A virtual call is resolved by the module's vtables only when the
    // module is the whole program: no file the pass does not see may
    // define a class derived from the call's.
    const bool wholeProgram = world.hasMain() && !world.openWorld() && !world.linked();

    LockSummaries locks;
    CodeCheck check(module, locks, uniformThreads, wholeProgram);
    recursiveWhy = recursionRisk(wholeProgram);
    llvm::SmallVector<StringRef, 2> names;
    StringRef(mutexOpt).split(names, ',', -1, false);
    for (StringRef n : names) {
        auto spec = parseSpec(n.trim(), module);
        if (!spec) {
            module.emitError() << "wtc: --wtc-delegate-sections: '" << n << "' is neither a global nor Class::member";
            return signalPassFailure();
        }
        llvm::SmallVector<cir::FuncOp> fs;
        for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
            if (!hasBody(f) || f->hasAttr(kSectionAttr)) continue;
            // A lock function of the program's own (a wrapper, or a lock
            // object's constructor) is the lock its callers' sections
            // begin with, not a section.
            const Summary &own = locks.of(f);
            if (own.net == 1 && own.events.size() == 1) continue;
            bool any = false;
            for (const LockSite &s : lockSites(f, *spec, locks)) any |= takes(s.ev);
            if (any) fs.push_back(f);
        }
        if (fs.empty()) {
            emitRemark(module.getLoc()) << "wtc: " << kIdiom << " mutex " << spec->text << " has no section here";
            continue;
        }
        for (cir::FuncOp f : fs) handle(f, *spec, locks, check);
    }
}

void DelegateSectionsPass::handle(cir::FuncOp f, const MutexSpec &spec, LockSummaries &locks, CodeCheck &check) {
    // The locks as written: flattening makes a call through which an
    // exception may pass a cir.try_call, which is no section's lock.
    llvm::SmallVector<Location> written;
    for (const LockSite &s : lockSites(f, spec, locks))
        if (takes(s.ev)) written.push_back(s.op->getLoc());
    // A flat graph of blocks, every variable at the entry, no dead blocks.
    {
        OpPassManager pm(cir::FuncOp::getOperationName());
        pm.addPass(createCIRFlattenCFGPass());
        pm.addPass(createHoistAllocasPass());
        if (failed(runPipeline(pm, f))) {
            f.emitError() << "wtc: --wtc-delegate-sections: cannot flatten " << shortName(f);
            return signalPassFailure();
        }
        IRRewriter rewriter(f.getContext());
        (void)eraseUnreachableBlocks(rewriter, f->getRegions());
    }
    llvm::SmallVector<LockSite> sites = lockSites(f, spec, locks);
    llvm::DenseMap<Operation *, const LockSite *> siteOf;
    for (const LockSite &s : sites) siteOf[s.op] = &s;
    for (Location loc : written)
        if (llvm::none_of(sites, [&](const LockSite &s) { return takes(s.ev) && s.op->getLoc() == loc; }))
            emitRemark(loc) << "wtc: " << kIdiom << " " << shortName(f) << " on " << spec.text
                            << " not delegated: (b) an exception may leave the section through its lock: it would "
                               "unwind the server, not the client";
    unsigned number = 0;
    llvm::DenseSet<Operation *> heldAt;  // locks met inside an earlier section
    for (const LockSite &lockSite : sites) {
        if (!takes(lockSite.ev)) continue;
        Operation *L = lockSite.op;
        if (L->getParentOfType<cir::FuncOp>() != f) continue;  // gone with an earlier section
        Verdict v;
        v.idiom = kIdiom.str();
        v.label = shortName(f) + " #" + std::to_string(++number) + " on " + spec.text;

        // A lock reached with the mutex already held, inside an earlier
        // section of this function: the caller would wait for itself.
        if (heldAt.contains(L)) v.refuse("(b) is reached with the mutex held, inside an earlier section");
        // The mutex itself must not be recursive.
        if (auto ptr = lockSite.key ? dyn_cast<cir::PointerType>(lockSite.key.getType()) : cir::PointerType()) {
            std::string ty = recordName(ptr.getPointee()).str();
            if (StringRef(ty).contains("recursive")) v.refuse("(c) the mutex is recursive (" + ty + ")");
        }
        // A read lock: readers share it as written, and the server would
        // run their sections one at a time, waiting in one for what another
        // reader holds.
        if (lockSite.ev == Ev::Shared)
            v.refuse("(c) takes the lock for reading: readers that share it as written would run one at a time "
                     "on the server");

        // The section's blocks: split before the lock and after each
        // unlock, then everything reachable from the lock without passing
        // an unlock.
        Block *pre = L->getBlock();
        Block *start = pre->splitBlock(L);
        OpBuilder b(f.getContext());
        b.setInsertionPointToEnd(pre);
        cir::BrOp::create(b, L->getLoc(), start);
        llvm::SetVector<Block *> blocks;
        llvm::SmallVector<std::pair<Block *, Block *>, 2> exits;  // (block ending at the unlock, its continuation)
        llvm::SmallVector<Block *> work{start};
        blocks.insert(start);
        bool shapeOk = true;
        while (!work.empty() && shapeOk) {
            Block *blk = work.pop_back_val();
            bool ended = false;
            for (Operation &op : llvm::make_early_inc_range(*blk)) {
                if (&op != L) {
                    auto it = siteOf.find(&op);
                    if (it != siteOf.end() && it->second->ev == Ev::Unlock) {
                        if (!it->second->same(lockSite)) {
                            v.refuse("(b) lets go a mutex of the same member that the pass cannot tell is the one "
                                     "it took");
                            shapeOk = false;
                            break;
                        }
                        Block *cont = blk->splitBlock(op.getNextNode());
                        b.setInsertionPointToEnd(blk);
                        cir::BrOp::create(b, op.getLoc(), cont);
                        exits.push_back({blk, cont});
                        ended = true;
                        break;
                    }
                    if (it != siteOf.end() && takes(it->second->ev)) {
                        heldAt.insert(&op);
                        v.refuse("(b) takes the mutex again before letting it go");
                        shapeOk = false;
                        break;
                    }
                } else if (blk != start) {
                    v.refuse("(b) reaches its own lock again before letting the mutex go");
                    shapeOk = false;
                    break;
                }
                if (isa<cir::ReturnOp>(op)) {
                    v.refuse("(b) leaves " + shortName(f) + " with the mutex held");
                    shapeOk = false;
                    break;
                }
                if (isa<cir::TryCallOp, cir::ResumeOp, cir::EhInitiateOp>(op) ||
                    op.getName().getStringRef().starts_with("cir.eh.")) {
                    std::string via;
                    if (auto tc = dyn_cast<cir::TryCallOp>(op); tc && tc.getCalleeAttr())
                        via = " through a call of " + llvm::demangle(tc.getCalleeAttr().getValue().str());
                    v.refuse("(b) an exception may leave the section" + via +
                             ": it would unwind the server, not the client");
                    shapeOk = false;
                    break;
                }
                if (isa<cir::GotoOp, cir::LabelOp>(op)) {
                    v.refuse("(b) jumps by goto");
                    shapeOk = false;
                    break;
                }
                // Variables are at the function's entry (cir-hoist-allocas)
                // but those of run-time size: on the server's stack, they
                // would be gone before the caller reads them after the unlock.
                if (isa<cir::AllocaOp, cir::StackSaveOp, cir::StackRestoreOp>(op))
                    v.refuse("(a) allocates stack memory of run-time size (a variable-length array or alloca), "
                             "which would be the server's stack");
                if (isa<cir::VAStartOp, cir::VAArgOp, cir::VAEndOp, cir::VACopyOp>(op))
                    v.refuse("(b) reads the variadic arguments of " + shortName(f));
            }
            if (!shapeOk || ended) continue;
            Operation *term = blk->empty() ? nullptr : &blk->back();
            if (!term || !term->hasTrait<OpTrait::IsTerminator>()) {
                v.refuse("(b) a block of the section has no terminator");
                shapeOk = false;
                break;
            }
            for (Block *s : term->getSuccessors())
                if (blocks.insert(s)) work.push_back(s);
        }
        if (shapeOk && exits.empty()) v.refuse("(b) never lets the mutex go in " + shortName(f));
        if (shapeOk) {
            // One entry: the lock.
            for (Block *blk : blocks)
                for (Block *p : blk->getPredecessors())
                    if (!blocks.contains(p) && !(blk == start && p == pre)) {
                        v.refuse("(b) is entered other than through its lock");
                        shapeOk = false;
                    }
            if (start->getSinglePredecessor() != pre && shapeOk) {
                v.refuse("(b) reaches its own lock again before letting the mutex go");
                shapeOk = false;
            }
        }
        // The section's own code: every op of its blocks (the exits' branch
        // to the continuation aside).
        llvm::SmallVector<Operation *> ops;
        llvm::DenseSet<Operation *> exitBranches;
        for (auto &[blk, cont] : exits) exitBranches.insert(&blk->back());
        for (Block *blk : blocks)
            for (Operation &op : *blk)
                if (!exitBranches.contains(&op)) ops.push_back(&op);
        if (shapeOk) {
            // No exception can leave it: the function is nothrow, or every
            // call it makes is.
            // The mutex's own lock and unlock are taken not to fail: a
            // std::mutex reports a failure to lock by throwing, which a
            // mutex used as the original uses it does not do.
            if (!f->hasAttr("nothrow"))
                for (Operation *op : ops)
                    if (auto c = dyn_cast<cir::CallOp>(op); c && !c.getNothrow() && op != L && !siteOf.count(op)) {
                        v.refuse("(b) an exception may leave the section through a call of " +
                                 (lookupCallee(c) ? shortName(lookupCallee(c)) : std::string("a function pointer")) +
                                 ": it would unwind the server, not the client");
                        break;
                    }
            // Its other locks balance, and it reaches no other section of
            // the same mutex.
            int net = 0;
            for (Operation *op : ops) {
                auto c = dyn_cast<cir::CallOp>(op);
                if (!c) continue;
                if (op == L) {
                    net += 1;
                    continue;
                }
                auto it = siteOf.find(op);
                if (it != siteOf.end()) {
                    // The exits' unlocks.
                    net -= 1;
                    continue;
                }
                llvm::SmallVector<std::pair<Ev, Target>, 2> evs;
                int callNet = 0;
                locks.eventsOf(c, evs, callNet);
                net += callNet;
            }
            // Each exit's unlock counted once per exit; the code has as
            // many unlocks as exits, each on its own path.
            net += static_cast<int>(exits.size()) - 1;
            if (net != 0)
                v.refuse("(c) takes a lock it does not let go within the section, or lets go one it did not take");
            llvm::DenseSet<Operation *> own{L};
            for (Operation *op : ops)
                if (siteOf.count(op)) own.insert(op);
            if (auto r = unlocksForeign(ops, own, locks)) v.refuse(*r);
            if (!recursiveWhy.empty() && check.reachesRawLock(ops))
                v.refuse("(c) takes a pthread or C11 mutex the pass cannot prove is not recursive: " + recursiveWhy);
            for (Operation *op : ops)
                if (auto r = check.at(op)) v.refuse(*r);
        }

        if (v.refused()) {
            for (const Reason &r : v.reasons)
                emitRemark(L->getLoc()) << "wtc: " << kIdiom << " " << v.label << " not delegated: " << r.str();
            continue;
        }
        if (!rewrite) {
            emitRemark(L->getLoc()) << "wtc: " << kIdiom << " " << v.label << " delegated";
            continue;
        }

        // ---- Outline.
        MLIRContext *ctx = f.getContext();
        Location loc = L->getLoc();
        auto voidPtr = cir::PointerType::get(cir::VoidType::get(ctx));
        auto s32 = cir::IntType::get(ctx, 32, /*isSigned=*/true);
        auto isInside = [&](Operation *op) {
            for (Operation *p = op; p; p = p->getParentOp())
                if (p->getBlock() && blocks.contains(p->getBlock()) && !exitBranches.contains(p)) return true;
            return false;
        };
        auto definedInside = [&](Value val) {
            if (auto ba = dyn_cast<BlockArgument>(val)) {
                Block *owner = ba.getOwner();
                if (blocks.contains(owner)) return true;
                Operation *parent = owner->getParentOp();
                return parent && parent != f.getOperation() && isInside(parent);
            }
            return isInside(val.getDefiningOp());
        };
        // A lifetime marker names a variable of its own function's frame;
        // the section's variables stay in the client's, so their markers,
        // hints for the optimiser, go.
        {
            llvm::SmallVector<Operation *> markers;
            for (Operation *op : ops)
                if (isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(op)) markers.push_back(op);
            llvm::erase_if(ops, [&](Operation *op) { return isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(op); });
            for (Operation *op : markers) op->erase();
        }
        llvm::SetVector<Value> inputs, outputs;
        for (Operation *op : ops)
            op->walk([&](Operation *o) {
                for (Value operand : o->getOperands())
                    if (!definedInside(operand)) inputs.insert(operand);
            });
        for (Block *blk : blocks) {
            for (BlockArgument a : blk->getArguments())
                for (Operation *user : a.getUsers())
                    if (!isInside(user)) outputs.insert(a);
            for (Operation &op : *blk) {
                if (exitBranches.contains(&op)) continue;
                for (Value r : op.getResults())
                    for (Operation *user : r.getUsers())
                        if (!isInside(user)) outputs.insert(r);
            }
        }
        llvm::SmallVector<Value> passed;  // inputs passed through the environment
        for (Value in : inputs)
            if (!in.getDefiningOp<cir::ConstantOp>()) passed.push_back(in);
        const unsigned slots = passed.size() + outputs.size();

        // The section's function.
        std::string name = f.getSymName().str() + ".wtc.section." + std::to_string(++outlined);
        OpBuilder mb = OpBuilder::atBlockEnd(module.getBody());
        auto fn = cir::FuncOp::create(mb, f.getLoc(), name, cir::FuncType::get({voidPtr}, s32),
                                      cir::GlobalLinkageKind::InternalLinkage);
        SymbolTable::setSymbolVisibility(fn, SymbolTable::Visibility::Private);
        fn->setAttr(kSectionAttr, StringAttr::get(ctx, spec.text));
        for (NamedAttribute a : f->getAttrs()) {
            StringRef an = a.getName().getValue();
            if (an.starts_with("cir.target") || an.starts_with("cir.tune") || an == "nothrow") fn->setAttr(a.getName(), a.getValue());
        }
        Block *entry = fn.addEntryBlock();
        OpBuilder fb = OpBuilder::atBlockBegin(entry);
        auto slotPtrTy = cir::PointerType::get(voidPtr);
        Value base = cir::CastOp::create(fb, loc, slotPtrTy, cir::CastKind::bitcast, entry->getArgument(0));
        auto slotAddr = [&](OpBuilder &bb, Value baseV, unsigned i) -> Value {
            Value idx = cir::ConstantOp::create(bb, loc, s32, cir::IntAttr::get(s32, i));
            return cir::PtrStrideOp::create(bb, loc, baseV.getType(), baseV, idx);
        };
        IRMapping inside;
        // A pointer travels in its environment slot itself, any other value
        // through a variable of the caller's that the slot points to: one
        // load of the caller's memory for most inputs, which are addresses
        // (the object, the caller's variables).
        auto direct = [](Value v) { return isa<cir::PointerType>(v.getType()); };
        for (unsigned i = 0; i < passed.size(); ++i) {
            Value p = cir::LoadOp::create(fb, loc, voidPtr, slotAddr(fb, base, i));
            if (direct(passed[i])) {
                inside.map(passed[i], p.getType() == passed[i].getType()
                                          ? p
                                          : cir::CastOp::create(fb, loc, passed[i].getType(), cir::CastKind::bitcast, p)
                                                .getResult());
                continue;
            }
            Value typed = cir::CastOp::create(fb, loc, cir::PointerType::get(passed[i].getType()),
                                              cir::CastKind::bitcast, p);
            inside.map(passed[i], cir::LoadOp::create(fb, loc, passed[i].getType(), typed));
        }
        for (Value in : inputs)
            if (auto c = in.getDefiningOp<cir::ConstantOp>()) inside.map(in, fb.clone(*c)->getResult(0));
        llvm::SmallVector<Value> outSlot;
        for (unsigned j = 0; j < outputs.size(); ++j) {
            Value p = cir::LoadOp::create(fb, loc, voidPtr, slotAddr(fb, base, passed.size() + j));
            outSlot.push_back(cir::CastOp::create(fb, loc, cir::PointerType::get(outputs[j].getType()),
                                                  cir::CastKind::bitcast, p));
        }
        cir::BrOp::create(fb, loc, start);
        for (Block *blk : blocks) blk->moveBefore(&fn.getBody(), fn.getBody().end());
        for (auto &[from, to] : inside.getValueMap()) {
            Value v0 = from;
            v0.replaceUsesWithIf(to, [&](OpOperand &u) { return fn->isAncestor(u.getOwner()); });
        }
        for (unsigned j = 0; j < outputs.size(); ++j) {
            Value o = outputs[j];
            OpBuilder ob(ctx);
            if (auto ba = dyn_cast<BlockArgument>(o)) ob.setInsertionPointToStart(ba.getOwner());
            else ob.setInsertionPointAfter(o.getDefiningOp());
            cir::StoreOp::create(ob, loc, o, outSlot[j]);
        }
        for (unsigned k = 0; k < exits.size(); ++k) {
            Block *blk = exits[k].first;
            blk->back().erase();
            OpBuilder eb = OpBuilder::atBlockEnd(blk);
            Value code = cir::ConstantOp::create(eb, loc, s32, cir::IntAttr::get(s32, k));
            cir::ReturnOp::create(eb, loc, ValueRange{code});
        }

        // The call, in place of the section.
        pre->back().erase();  // the branch to the section
        Block &fentry = f.getBody().front();
        OpBuilder ab = OpBuilder::atBlockBegin(&fentry);
        auto align8 = ab.getI64IntegerAttr(8);
        auto envTy = cir::ArrayType::get(voidPtr, std::max(1u, slots));
        Value env = cir::AllocaOp::create(ab, loc, cir::PointerType::get(envTy), "wtc.env", align8);
        llvm::SmallVector<Value> inSlots, outSlots;
        for (Value in : passed)
            inSlots.push_back(direct(in) ? Value()
                                         : cir::AllocaOp::create(ab, loc, cir::PointerType::get(in.getType()), "wtc.in",
                                                                 align8)
                                               .getResult());
        for (Value o : outputs)
            outSlots.push_back(cir::AllocaOp::create(ab, loc, cir::PointerType::get(o.getType()), "wtc.out", align8));
        OpBuilder cb = OpBuilder::atBlockEnd(pre);
        Value envBase = cir::CastOp::create(cb, loc, slotPtrTy, cir::CastKind::array_to_ptrdecay, env);
        for (unsigned i = 0; i < passed.size(); ++i) {
            Value carried = passed[i];
            if (!direct(carried)) {
                cir::StoreOp::create(cb, loc, carried, inSlots[i]);
                carried = inSlots[i];
            }
            Value asVoid = carried.getType() == voidPtr
                               ? carried
                               : cir::CastOp::create(cb, loc, voidPtr, cir::CastKind::bitcast, carried).getResult();
            cir::StoreOp::create(cb, loc, asVoid, slotAddr(cb, envBase, i));
        }
        for (unsigned j = 0; j < outputs.size(); ++j) {
            Value asVoid = cir::CastOp::create(cb, loc, voidPtr, cir::CastKind::bitcast, outSlots[j]);
            cir::StoreOp::create(cb, loc, asVoid, slotAddr(cb, envBase, passed.size() + j));
        }
        Value envVoid = cir::CastOp::create(cb, loc, voidPtr, cir::CastKind::bitcast, env);
        Value key = lockSite.key;
        if (!key) {
            auto g = module.lookupSymbol<cir::GlobalOp>(lockSite.global);
            key = cir::GetGlobalOp::create(cb, loc, cir::PointerType::get(g.getSymType()), g.getSymName());
        }
        auto del = DelegateOp::create(cb, loc, s32, key, envVoid, FlatSymbolRefAttr::get(fn.getSymNameAttr()),
                                      StringAttr::get(ctx, spec.text));
        for (unsigned j = 0; j < outputs.size(); ++j) {
            Value back = cir::LoadOp::create(cb, loc, outputs[j].getType(), outSlots[j]);
            Value o = outputs[j];
            o.replaceUsesWithIf(back, [&](OpOperand &u) { return !fn->isAncestor(u.getOwner()); });
        }
        if (exits.size() == 1) {
            cir::BrOp::create(cb, loc, exits[0].second);
        } else {
            llvm::SmallVector<llvm::APInt> values;
            llvm::SmallVector<Block *> dests;
            llvm::SmallVector<ValueRange> operands;
            for (unsigned k = 1; k < exits.size(); ++k) {
                values.push_back(llvm::APInt(32, k));
                dests.push_back(exits[k].second);
                operands.push_back(ValueRange{});
            }
            cir::SwitchFlatOp::create(cb, loc, del.getExit(), exits[0].second, ValueRange{}, values, dests, operands);
        }
        emitRemark(L->getLoc()) << "wtc: " << kIdiom << " " << v.label << " delegated";
    }
}

}  // namespace

namespace mlir::wtc {
void registerWTCDelegateSectionsPass() { PassRegistration<DelegateSectionsPass>(); }
}  // namespace mlir::wtc
