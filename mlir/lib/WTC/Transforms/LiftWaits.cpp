#include "WTC/Transforms/LiftWaits.h"
#include "WTC/Transforms/ProgressRequirements.h"
#include "WTC/Transforms/LiftNames.h"
#include "WTC/Transforms/Refusal.h"
#include "WTC/Transforms/ThreadInstances.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/SymbolTable.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringMap.h"

#include <optional>

using namespace mlir;
using namespace mlir::wtc;

// The conditions, each named in the remark that refuses on it
// (docs/lifting.md, "Lifting a wait"):
//
//   (g) a closed module: it has main, nothing is linked with it and it is
//       not compiled to be linked later, and every member function of the
//       class has its body here;
//   (h) the condition variable is constructed, destroyed, notified and
//       waited on, with a predicate, at one place, and used for nothing
//       else; the wait is std::condition_variable::wait's predicate loop;
//   (i) the mutex is constructed and locked by std::unique_lock and
//       std::lock_guard objects, and used for nothing else; each lock
//       object is only built and destroyed, the wait's also waited with;
//   (j) the predicate reads only a queue whose operations are atomic on
//       their own (a queue class the pass read, by its try-pop), atomic
//       flags (std::atomic<bool>, by loads), its captures and the waiting
//       function's locals it captures by reference, which nothing else
//       can reach; it writes only those locals;
//   (k) the wait's section, which runs without the mutex once lifted,
//       reads and writes only the waiting function's locals and changes
//       the class's state by at most one atomic read-modify-write on each
//       side of the wait, of a member the predicate does not read, whose
//       address goes only to its atomic operations and which nothing
//       else touches in a section or a function that notifies; every
//       other section of the mutex, which keeps it, changes nothing the
//       predicate reads, at any depth of the calls it makes;
//   (l) no object of the class is copied whole or read as another type,
//       and no pointer to a member of it is formed; no pointer to an
//       object that holds one is converted to another type or handed to
//       a function with no body here, and no union that holds one is
//       read through another member.

namespace {

bool isCondVar(Type t) { return recordName(t) == "std::condition_variable"; }
bool isMutex(Type t) { return recordName(t) == "std::mutex"; }

std::string contextOf(cir::FuncOp f) { return f ? squashSpaces(nameOf(f.getSymName()).context) : std::string(); }
std::string baseOf(cir::FuncOp f) { return f ? nameOf(f.getSymName()).base : std::string(); }

// The member of `rec` an address names, when it is a member step on a
// pointer to `rec`.
std::optional<unsigned> memberOf(Value addr, cir::RecordType rec) {
    while (auto c = addr.getDefiningOp<cir::BaseClassAddrOp>()) addr = c.getOperand();
    auto gm = addr.getDefiningOp<cir::GetMemberOp>();
    if (!gm) return std::nullopt;
    auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
    if (!pt || pt.getPointee() != Type(rec)) return std::nullopt;
    return gm.getIndex();
}

// The value an address steps into: through member steps, base class steps
// and casts that keep the object.
Value baseOfAddress(Value v) {
    while (true) {
        if (auto gm = v.getDefiningOp<cir::GetMemberOp>()) v = gm.getAddr();
        else if (auto b = v.getDefiningOp<cir::BaseClassAddrOp>()) v = b.getOperand();
        else if (auto c = v.getDefiningOp<cir::CastOp>(); c && c.getKind() == cir::CastKind::bitcast) v = c.getSrc();
        else return v;
    }
}

// For a remark: "Holder, which holds Pool," or the class's own name.
std::string holderName(Type t, cir::RecordType rec, const std::string &cls) {
    if (t == Type(rec)) return cls;
    if (auto r = dyn_cast<cir::RecordType>(t); r && r.getName())
        return recordName(r).str() + ", which holds " + cls + ",";
    return "an object that holds " + cls + ",";
}

// Whether a pointer goes only to free and operator delete: an object
// destroyed and its storage given back, not read.
bool onlyFreed(Value v) {
    return !v.use_empty() && llvm::all_of(v.getUses(), [](OpOperand &u) {
        auto call = dyn_cast<cir::CallOp>(u.getOwner());
        cir::FuncOp g = call ? lookupCallee(call) : cir::FuncOp();
        if (!g) return false;
        StringRef n = g.getSymName();
        return n == "free" || n.starts_with("_ZdlPv") || n.starts_with("_ZdaPv");
    });
}

bool atomicBoolType(StringRef ctx) { return ctx == "std::atomic<bool>" || ctx == "std::__atomic_base<bool>"; }
bool atomicType(StringRef ctx) { return ctx.starts_with("std::atomic<") || ctx.starts_with("std::__atomic_base<"); }
bool atomicRead(StringRef base) { return base == "load" || base == "operatorbool" || base == "operator bool"; }

// What a function with no body here may do, for a section that keeps the
// mutex (condition (k)): nothing a predicate reads (Pure); that, and call
// the program's own operator new or delete where it replaces the
// library's, and its new-handler (Allocating: libstdc++'s strings and
// exception classes allocate); that, and run the destructor of any object
// the program throws, and its terminate handler (Exceptional: throwing,
// catching, rethrowing, dropping an exception pointer's last reference);
// or anything (Unknown, every function not listed).
enum class External { Pure, Allocating, Exceptional, Unknown };

External classifyExternal(cir::FuncOp f) {
    StringRef sym = f.getSymName();
    Name n = nameOf(sym);
    // Memory functions: under the contract (no undefined behaviour) they
    // write no std::atomic<bool> and, by condition (a) of the queue class,
    // not its container. Other mutexes and condition variables, taken and
    // woken, change no data of the program's.
    for (const char *x : {"memcpy", "memmove", "memset", "memcmp", "strlen", "pthread_mutex_lock",
                          "pthread_mutex_unlock", "pthread_mutex_trylock", "pthread_cond_signal",
                          "pthread_cond_broadcast", "sched_yield", "abort", "__assert_fail",
                          "__cxa_allocate_exception", "__cxa_begin_catch", "__cxa_init_primary_exception",
                          "__cxa_pure_virtual"})
        if (sym == x) return External::Pure;
    for (const char *x : {"__cxa_throw", "__cxa_rethrow", "__cxa_end_catch", "__cxa_free_exception", "_Unwind_Resume"})
        if (sym == x) return External::Exceptional;
    if (!n.ok) return External::Unknown;
    StringRef ctx(n.context);
    // libstdc++'s futex for futures, and the categories of its own errors.
    if (ctx == "std::__atomic_futex_unsigned_base") return External::Pure;
    if ((ctx == "std" || ctx == "std::_V2") &&
        (n.base == "future_category" || n.base == "system_category" || n.base == "generic_category"))
        return External::Pure;
    // A new reference to the exception being handled; dropping one is an
    // exception pointer's, below.
    if (ctx == "std" && n.base == "current_exception") return External::Pure;
    if (ctx.empty() && (n.base == "operator new" || n.base == "operator delete" || n.base == "operator new[]" ||
                        n.base == "operator delete[]"))
        return External::Allocating;
    if (ctx.starts_with("std::__cxx11::basic_string<") || ctx.starts_with("std::basic_string<")) return External::Allocating;
    for (const char *c : {"std::exception", "std::logic_error", "std::runtime_error", "std::system_error",
                          "std::future_error", "std::bad_alloc", "std::bad_function_call", "std::invalid_argument",
                          "std::length_error", "std::out_of_range"})
        if (ctx == c) return External::Allocating;
    if (ctx == "std" && StringRef(n.base).starts_with("__throw_")) return External::Exceptional;
    if (ctx == "std::__exception_ptr::exception_ptr" || ctx == "std::__future_base::_Result_base") return External::Exceptional;
    if (ctx == "std" && (n.base == "rethrow_exception" || n.base == "terminate")) return External::Exceptional;
    return External::Unknown;
}

enum class Capture { This, Local, AtomicRef };

struct Wait {
    cir::RecordType rec;
    unsigned cv = 0, mutex = 0;
    std::string cvName, mutexName;
    Verdict v;
    Location loc;
    llvm::SmallVector<cir::CallOp, 2> cvCtors, cvDtors, notifyOne, notifyAll, waits, locks;
    cir::CallOp waitCall, waitLock;
    cir::FuncOp waitFn, waitBody, pred;
    Value lockSlot;
    llvm::DenseSet<unsigned> predAtomics;  // members of the class the predicate reads atomically
    // The types of the atomics it reads that are not known members (a heap
    // flag through a captured reference): any atomic of such a type may be one.
    llvm::SmallVector<Type, 2> predAtomicTypes;
    llvm::DenseSet<unsigned> waitWrites;  // members the waiting section changes
    const QueueClassForWaits *popped = nullptr;  // the queue the predicate pops
    // The wait's two ends, for its policy (shim/lifted.hpp): the threads
    // that push the queue, those that wait, 0 when not known; the waiters
    // read at run time from a slot of the function that builds the class;
    // whether a producer provably sleeps or blocks before each push.
    int producers = 0, waiters = 0;
    cir::AllocaOp waitersSlot;
    bool paced = false;
    // A waiter on a path with a requirement of progress (a real-time
    // thread, a declared one): the policy must never spin past its pause.
    bool noSpin = false;
    std::string ends;
    explicit Wait(Location l) : loc(l) {}
    std::string fn(Operation *op) const { return shortName(op->getParentOfType<cir::FuncOp>()); }
};

class Lifter {
public:
    Lifter(ModuleOp m, const ClosedWorld &w, llvm::ArrayRef<QueueClassForWaits> q) : module(m), world(w), queues(q) {}

    std::string run(bool rewrite) {
        RequirementRoots requirements(module);
        llvm::SetVector<cir::RecordType> recs;
        module.walk([&](cir::GetMemberOp gm) {
            auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
            auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
            if (!rec || rec.isIncomplete() || !isCondVar(rec.getElementType(gm.getIndex()))) return;
            if (llvm::any_of(queues, [&](const QueueClassForWaits &qc) { return qc.rec == rec; })) return;
            recs.insert(rec);
        });
        std::string lines;
        for (cir::RecordType rec : recs) {
            Wait w(module.getLoc());
            w.rec = rec;
            if (!read(w)) continue;  // no wait to lift
            if (w.v.refused()) {
                w.v.emitRefusals(w.loc);
                continue;
            }
            // Progress (docs/progress.md, "What a requirement refuses"). The
            // policy may spin and then yield before it parks, which a thread
            // at real-time priority, or one under a declared requirement,
            // must not do: a yield keeps a producer of lower priority off
            // its processor. Such a wait is lifted with the policy that
            // parks after its pause, as the original's condition variable
            // sleeps at once. A wait a signal handler reaches is not lifted
            // at all: the original's wait was already the handler's to
            // answer for, and the eventcount's state, which the handler
            // could reenter on the thread it interrupted inside the same
            // wait, is not proven safe for that; keeping the original keeps
            // whatever the program had.
            {
                llvm::SmallVector<cir::CallOp, 4> chain;
                // A handler anywhere among the requirements decides first.
                const RequirementRoot *r = requirements.reaching(
                    w.waitFn, &chain, [](const RequirementRoot &q) { return q.source == RequirementRoot::Source::handler; });
                if (!r) r = requirements.reaching(w.waitFn, &chain);
                if (r) {
                    std::string via;
                    for (cir::CallOp c : chain)
                        if (cir::FuncOp t = lookupCallee(c)) via += (via.empty() ? " through " : ", ") + shortName(t);
                    cir::FuncOp rf = r->func;
                    const std::string from = shortName(rf) + via + ", " + r->why;
                    if (r->source == RequirementRoot::Source::handler) {
                        w.v.emitRefusal(w.loc, "the wait is reached from " + from +
                                                   ": the eventcount is not proven safe to reenter from a handler, "
                                                   "so the original wait is kept");
                        continue;
                    }
                    w.noSpin = true;
                    w.ends += "; it parks after its pause, never spinning on, since a waiter is reached from " + from;
                }
            }
            llvm::StringMap<cir::FuncOp> rt = runtimeFor(rec);
            if (rt.empty()) {
                StringRef ty = recordName(rec);
                if (ty.contains("(lambda") || ty.contains("(anonymous") || ty.contains("(unnamed")) {
                    w.v.emitRefusal(w.loc, "its type cannot be named outside the file");
                    continue;
                }
                lines += ("template struct wtc::lifted_wait<" + ty + ">;\n").str();
                if (rewrite)
                    w.v.emitRefusal(w.loc, "wtc::lifted_wait<" + ty + "> is not instantiated in this module (wtc-cc "
                                           "instantiates it)");
                else
                    w.v.emitLifts(w.loc);
                continue;
            }
            if (rewrite) lift(w, rt);
            else w.v.emitLifts(w.loc);
        }
        return lines;
    }

private:
    ModuleOp module;
    const ClosedWorld &world;
    llvm::ArrayRef<QueueClassForWaits> queues;
    unsigned clones = 0;

    const QueueClassForWaits *queueOf(cir::FuncOp f) const {
        for (const QueueClassForWaits &q : queues)
            if (q.functions.contains(f)) return &q;
        return nullptr;
    }

    // Reads the class's uses of its condition variable and mutex; false
    // when it has no wait with a predicate (nothing to lift, nothing said).
    bool read(Wait &w) {
        llvm::ArrayRef<Type> members = w.rec.getMembers();
        std::optional<unsigned> cv, mx;
        for (unsigned i = 0; i < members.size(); ++i) {
            if (isCondVar(members[i])) {
                if (cv) return false;  // two: not this shape
                cv = i;
            }
            if (isMutex(members[i])) {
                if (mx) return false;
                mx = i;
            }
        }
        if (!cv || !mx) return false;
        w.cv = *cv;
        w.mutex = *mx;
        const std::string cls = recordName(w.rec).str();

        // (h), (i): every use of the two members.
        bool sawWait = false;
        module.walk([&](cir::GetMemberOp gm) {
            auto m = memberOf(gm.getResult(), w.rec);
            if (!m || (*m != w.cv && *m != w.mutex)) return;
            if (*m == w.cv && w.cvName.empty()) w.cvName = gm.getName().str();
            if (*m == w.mutex && w.mutexName.empty()) w.mutexName = gm.getName().str();
            for (OpOperand &use : gm.getResult().getUses()) {
                auto call = dyn_cast<cir::CallOp>(use.getOwner());
                cir::FuncOp g = call ? lookupCallee(call) : cir::FuncOp();
                const std::string where = shortName(gm->getParentOfType<cir::FuncOp>());
                if (*m == w.cv) {
                    if (!g || use.getOperandNumber() != 0 || contextOf(g) != "std::condition_variable") {
                        w.v.refuse("(h) " + where + " uses " + gm.getName() +
                                   " otherwise than by its member functions");
                        continue;
                    }
                    Name n = nameOf(g.getSymName());
                    if (n.ctor) w.cvCtors.push_back(call);
                    else if (n.dtor) w.cvDtors.push_back(call);
                    else if (n.base == "notify_one") w.notifyOne.push_back(call);
                    else if (n.base == "notify_all") w.notifyAll.push_back(call);
                    else if (n.base == "wait" && call.getNumOperands() >= 3 && !g.getBody().empty()) {
                        w.waits.push_back(call);
                        sawWait = true;
                    } else if (n.base == "wait") {
                        w.v.refuse("(h) " + where + " waits on " + gm.getName() + " without a predicate");
                        sawWait = true;
                    } else if (n.base == "wait_for" || n.base == "wait_until") {
                        w.v.refuse("(h) " + where + " waits on " + gm.getName() + " with a time limit");
                        sawWait = true;
                    } else {
                        w.v.refuse("(h) " + where + " calls " + gm.getName() + "." + n.base);
                    }
                } else {
                    const std::string ctx = g ? contextOf(g) : std::string();
                    Name n = g ? nameOf(g.getSymName()) : Name();
                    if (g && ctx == "std::mutex" && (n.ctor || n.dtor) && use.getOperandNumber() == 0) continue;
                    if (g && n.ctor && call.getNumOperands() == 2 && use.getOperandNumber() == 1 &&
                        (ctx == "std::unique_lock<std::mutex>" || ctx == "std::lock_guard<std::mutex>")) {
                        w.locks.push_back(call);
                        continue;
                    }
                    w.v.refuse("(i) " + where + " uses " + gm.getName() +
                               " otherwise than by a std::unique_lock or std::lock_guard");
                }
            }
        });
        if (!sawWait) return false;
        w.v.idiom = "locked wait";
        w.v.label = (w.cvName.empty() ? std::string("<cv>") : w.cvName) + " in " + cls;
        if (!w.waits.empty()) w.loc = w.waits.front().getLoc();
        if (w.waits.size() > 1) w.v.refuse("(h) " + w.cvName + " is waited on at " + std::to_string(w.waits.size()) + " places");
        if (w.v.refused()) return true;

        closedWorld(w, cls);
        wholeObjects(w, cls);
        if (w.v.refused()) return true;
        waitShape(w);
        if (w.v.refused()) return true;
        predicate(w);
        if (w.v.refused()) return true;
        sections(w);
        if (!w.v.refused()) countEnds(w);
        return true;
    }

    // A call that makes its thread wait for something outside it: a sleep,
    // a blocking read, a wait on another thread.
    static bool paces(cir::FuncOp g) {
        if (!g) return false;
        Name n = nameOf(g.getSymName());
        StringRef base = n.ok ? StringRef(n.base) : g.getSymName();
        static const char *const names[] = {"sleep", "usleep", "nanosleep", "clock_nanosleep", "__sleep_for",
                                            "sleep_for", "sleep_until", "read", "recv", "recvfrom", "recvmsg",
                                            "poll", "select", "epoll_wait", "accept", "fread", "fgets", "getline",
                                            "getchar", "scanf", "fscanf", "sem_wait", "pthread_cond_wait",
                                            "pthread_join", "join", "wait", "wait_for", "wait_until", "get"};
        for (const char *x : names)
            if (base == x) return true;
        return false;
    }

    // Whether a call runs on every iteration of the innermost loop around
    // it, after a pacing call that also does: a pacing call in the same
    // loop body, outside any branch of it.
    static bool pacedBefore(Operation *call) {
        Operation *loop = call->getParentOp();
        while (loop && !isa<cir::LoopOpInterface>(loop) && !isa<cir::FuncOp>(loop)) loop = loop->getParentOp();
        if (!loop || isa<cir::FuncOp>(loop)) return false;
        bool found = false;
        loop->walk([&](cir::CallOp c) {
            if (found || !paces(lookupCallee(c))) return;
            for (Operation *p = c->getParentOp(); p && p != loop; p = p->getParentOp())
                if (isa<cir::IfOp, cir::TernaryOp, cir::SwitchOp, cir::CaseOp>(p) || isa<cir::LoopOpInterface>(p)) return;
            found = true;
        });
        return found;
    }

    // The threads at the wait's two ends (ThreadInstances.h): a policy the
    // eventcount chooses when the class is built (shim/lifted.hpp). Only
    // performance depends on it: every policy is a complete wait.
    void countEnds(Wait &w) {
        if (!w.popped || w.popped->pushes.empty()) {
            w.ends = "producers unknown (the predicate pops no queue whose pushes the pass knows)";
            return;
        }
        ThreadInstanceWalk *self = nullptr;
        Operation *waitOp = w.waitCall.getOperation();
        const QueueClassForWaits *q = w.popped;
        auto relevant = [&](cir::FuncOp f) {
            return self->reachesOp(f, [&](Operation *op) {
                if (op == waitOp) return true;
                auto c = dyn_cast<cir::CallOp>(op);
                return c && q->pushes.contains(lookupCallee(c));
            });
        };
        ThreadInstanceWalk walk(module, relevant);
        self = &walk;
        llvm::SmallVector<Instance *, 8> waiting, pushing;
        walk.run(
            [&](Instance &inst) {
                if (inst.func == w.waitFn) waiting.push_back(&inst);
                bool pushes = false;
                inst.func.walk([&](cir::CallOp c) { pushes |= q->pushes.contains(lookupCallee(c)); });
                if (pushes) pushing.push_back(&inst);
            },
            [&](cir::FuncOp) {});
        // Distinct threads, each one, or nothing known.
        auto count = [](llvm::ArrayRef<Instance *> is) -> int {
            if (is.empty()) return 0;
            llvm::DenseSet<unsigned> threads;
            for (Instance *i : is) {
                if (i->speculative || i->threadMult != Mult::One) return 0;
                threads.insert(i->thread);
            }
            return static_cast<int>(threads.size());
        };
        w.producers = count(pushing);
        w.waiters = count(waiting);
        // Waiters started by one counted loop, whose bound the function
        // that builds the class passes from a parameter it keeps: that
        // parameter, read when the condition variable is built.
        if (!w.waiters && !waiting.empty() && !w.cvCtors.empty()) {
            // The loop: the instance's own, or the first loop around the
            // spawn that starts its thread, or around a call on the way to
            // the spawn, up to three calls up.
            auto loopOf = [](Instance *i) -> Operation * {
                if (i->countLoop) return i->countLoop;
                Instance *root = i;
                while (root->parent && root->parent->thread == root->thread) root = root->parent;
                Operation *site = root->site;
                Instance *at = root->parent;
                for (unsigned hop = 0; site && hop < 3; ++hop) {
                    for (Operation *p = site->getParentOp(); p && !isa<cir::FuncOp>(p); p = p->getParentOp())
                        if (isa<cir::LoopOpInterface>(p)) return p;
                    if (!at) break;
                    site = at->site;
                    at = at->parent;
                }
                return nullptr;
            };
            Operation *loop = loopOf(waiting.front());
            bool same = loop && llvm::all_of(waiting, [&](Instance *i) { return loopOf(i) == loop && !i->speculative; });
            auto forLoop = same ? dyn_cast<cir::ForOp>(loop) : cir::ForOp();
            // Its bound: `i < n` or `i <= n` in its condition, n loaded from a
            // slot. A loop that starts above zero starts fewer threads; the
            // bound is what the policy reads (a class built before it runs
            // any, as a pool's constructor does).
            cir::LoadOp bound;
            if (forLoop && forLoop.getCond().hasOneBlock())
                if (auto cond = dyn_cast<cir::ConditionOp>(forLoop.getCond().front().getTerminator()))
                    if (auto cmp = cond.getCondition().getDefiningOp<cir::CmpOp>()) {
                        if (cmp.getKind() == cir::CmpOpKind::lt || cmp.getKind() == cir::CmpOpKind::le)
                            bound = cmp.getRhs().getDefiningOp<cir::LoadOp>();
                        else if (cmp.getKind() == cir::CmpOpKind::gt || cmp.getKind() == cir::CmpOpKind::ge)
                            bound = cmp.getLhs().getDefiningOp<cir::LoadOp>();
                    }
            cir::FuncOp loopFn = forLoop ? forLoop->getParentOfType<cir::FuncOp>() : cir::FuncOp();
            cir::FuncOp builder = w.cvCtors.front()->getParentOfType<cir::FuncOp>();
            std::optional<unsigned> boundParam;
            if (bound)
                if (auto slot = bound.getAddr().getDefiningOp<cir::AllocaOp>())
                    for (OpOperand &u : slot.getResult().getUses())
                        if (auto st = dyn_cast<cir::StoreOp>(u.getOwner()); st && st.getAddr() == slot.getResult())
                            if (auto a = dyn_cast<BlockArgument>(st.getValue()); a && a.getOwner()->getParentOp() == loopFn)
                                boundParam = a.getArgNumber();
            llvm::SmallVector<cir::CallOp, 2> calls;
            if (boundParam && builder)
                builder.walk([&](cir::CallOp c) {
                    if (lookupCallee(c) == loopFn) calls.push_back(c);
                });
            if (calls.size() == 1 && *boundParam < calls.front().getNumOperands())
                if (auto ld = calls.front().getOperand(*boundParam).getDefiningOp<cir::LoadOp>())
                    if (auto slot = ld.getAddr().getDefiningOp<cir::AllocaOp>(); slot && slot->getParentOfType<cir::FuncOp>() == builder) {
                        bool param = false, other = false;
                        for (OpOperand &u : slot.getResult().getUses()) {
                            if (isa<cir::LoadOp>(u.getOwner())) continue;
                            auto st = dyn_cast<cir::StoreOp>(u.getOwner());
                            if (st && st.getAddr() == slot.getResult() && isa<BlockArgument>(st.getValue())) param = true;
                            else other = true;
                        }
                        if (param && !other && llvm::all_of(w.cvCtors, [&](cir::CallOp c) {
                                return c->getParentOfType<cir::FuncOp>() == builder;
                            }) && isa<cir::IntType>(slot.getAllocaType()))
                            w.waitersSlot = slot;
                    }
        }
        for (Instance *i : pushing) {
            // Up the producer's chain within its thread: a call on the way
            // made after a pacing call on every iteration.
            for (Instance *at = i; at && at->thread == i->thread; at = at->parent)
                if (at->site && pacedBefore(at->site)) w.paced = true;
        }
        auto say = [](int n, const char *what) {
            return n ? std::to_string(n) + " " + what : std::string(what) + " not known";
        };
        w.ends = say(w.producers, "producers") + ", " +
                 (w.waitersSlot ? std::string("waiters read when it is built") : say(w.waiters, "waiters")) +
                 (w.paced ? ", a producer paced" : "");
    }

    // (g)
    void closedWorld(Wait &w, const std::string &cls) {
        if (world.openWorld())
            w.v.refuse("(g) the unit is compiled to be linked with files the pass does not see, which may use " + cls +
                       " as written");
        if (!world.hasMain()) w.v.refuse("(g) the module has no main: the rest of the program may use " + cls + " as written");
        if (world.linked())
            w.v.refuse("(g) files are linked with this one, whose code may use the condition variable of " + cls +
                       " as written");
        for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
            if (!f.getBody().empty() || f.getAliasee() || !isMemberOf(f, w.rec)) continue;
            w.v.refuse("(g) " + shortName(f) + " has no body here: another unit defines it");
        }
    }

    // (l)
    void wholeObjects(Wait &w, const std::string &cls) {
        module.walk([&](Operation *op) {
            auto f = op->getParentOfType<cir::FuncOp>();
            const std::string where = f ? shortName(f) : std::string("a global");
            if (auto cp = dyn_cast<cir::CopyOp>(op)) {
                auto pt = dyn_cast<cir::PointerType>(cp.getDst().getType());
                if (pt && holdsByValue(pt.getPointee(), w.rec)) w.v.refuse("(l) " + where + " copies an object of " + cls + " whole");
            }
            // The front end emits std::memcpy, and __builtin_memcpy too, as a
            // call of the C library's function, not as cir.libc.memcpy.
            auto bytesCall = [](Operation *o) {
                auto call = dyn_cast<cir::CallOp>(o);
                cir::FuncOp g = call ? lookupCallee(call) : cir::FuncOp();
                if (!g || !g.getBody().empty()) return false;
                StringRef n = g.getSymName();
                return n == "memcpy" || n == "memmove" || n == "memset";
            };
            if (isa<cir::MemCpyOp, cir::MemMoveOp, cir::MemSetOp>(op) || bytesCall(op))
                for (Value v : op->getOperands())
                    if (auto c = v.getDefiningOp<cir::CastOp>())
                        if (auto pt = dyn_cast<cir::PointerType>(c.getSrc().getType()); pt && holdsByValue(pt.getPointee(), w.rec))
                            w.v.refuse("(l) " + where + " copies or clears an object of " + cls + " as bytes");
            // An object of the class, or one that holds it, outside the
            // class: its address made a pointer to a type that does not
            // hold the class, or handed to a function with no body here,
            // directly or through a pointer that may reach one, or a union
            // that holds it read through another member, reaches the lifted
            // condition variable's storage where the pass does not follow
            // it. The module has no file linked (g), so code with no body
            // here is the system's. A pointer given only to free or
            // operator delete is the object's storage given back. The rule
            // sees the conversion, the call and the member step, not what a
            // pointer becomes after (docs/lifting.md, "Known limitations").
            // The escape analysis would follow it, but, field-insensitive,
            // it takes the pool for one class with whatever its workers'
            // captures reach, and refuses every pool whose program prints a
            // count, CTPL's included.
            if (auto c = dyn_cast<cir::CastOp>(op); c && c.getKind() != cir::CastKind::ptr_to_bool) {
                auto from = dyn_cast<cir::PointerType>(c.getSrc().getType());
                auto to = dyn_cast<cir::PointerType>(c.getResult().getType());
                if (from && holdsByValue(from.getPointee(), w.rec) && (!to || !holdsByValue(to.getPointee(), w.rec)) &&
                    !onlyFreed(c.getResult()))
                    w.v.refuse("(l) " + where + " converts a pointer to " + holderName(from.getPointee(), w.rec, cls) +
                               " to another type");
            }
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                std::optional<std::string> to;  // where the call may go that has no body here
                if (!call.isIndirect()) {
                    cir::FuncOp g = lookupCallee(call);
                    if (g && g.getBody().empty()) to = shortName(g) + ", which has no body here";
                } else {
                    if (!graphDone) buildGraph();
                    llvm::SmallVector<cir::FuncOp, 4> ts = targets(call);
                    if (ts.empty()) to = "a call through a pointer the pass cannot resolve";
                    for (cir::FuncOp g : ts)
                        if (!to && g.getBody().empty())
                            to = shortName(g) + " through a pointer, which has no body here";
                }
                if (to)
                    for (Value a : call.getArgOperands())
                        if (auto pt = dyn_cast<cir::PointerType>(a.getType()); pt && holdsByValue(pt.getPointee(), w.rec)) {
                            w.v.refuse("(l) " + where + " hands a pointer to " + holderName(pt.getPointee(), w.rec, cls) +
                                       " to " + *to);
                            break;
                        }
            }
            // A step into another member of a union that holds the class
            // reaches its storage as that member's type.
            if (auto gm = dyn_cast<cir::GetMemberOp>(op)) {
                auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
                auto u = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
                if (u && u.isUnion() && !u.isIncomplete() && gm.getIndex() < u.getMembers().size() &&
                    holdsByValue(u, w.rec) && !holdsByValue(u.getMembers()[gm.getIndex()], w.rec))
                    w.v.refuse("(l) " + where + " reads a union that holds " + cls + " through another of its members");
            }
            for (NamedAttribute a : op->getAttrs())
                a.getValue().walk([&](cir::DataMemberAttr dm) {
                    if (auto t = dyn_cast<cir::DataMemberType>(dm.getType()); t && t.getClassTy() == Type(w.rec))
                        w.v.refuse("(l) " + where + " forms a pointer to a member of " + cls);
                });
        });
    }

    // (h), (i): the wait's call, its lock and std::condition_variable::wait's loop.
    void waitShape(Wait &w) {
        w.waitCall = w.waits.front();
        w.waitFn = w.waitCall->getParentOfType<cir::FuncOp>();
        w.waitBody = lookupCallee(w.waitCall);
        const std::string where = shortName(w.waitFn);
        w.lockSlot = traceToLocalRoot(w.waitCall.getOperand(1));
        for (cir::CallOp l : w.locks)
            if (traceToLocalRoot(l.getOperand(0)) == w.lockSlot && l->getParentOfType<cir::FuncOp>() == w.waitFn &&
                contextOf(lookupCallee(l)) == "std::unique_lock<std::mutex>")
                w.waitLock = l;
        if (!w.waitLock || !w.lockSlot.getDefiningOp<cir::AllocaOp>()) {
            w.v.refuse("(i) " + where + " waits with a lock that is not a std::unique_lock it builds on " + w.mutexName);
            return;
        }
        for (OpOperand &use : w.lockSlot.getUses()) {
            Operation *u = use.getOwner();
            if (u == w.waitLock || u == w.waitCall || isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
            if (auto c = dyn_cast<cir::CallOp>(u); c && nameOf(lookupCallee(c).getSymName()).dtor &&
                                                   contextOf(lookupCallee(c)) == "std::unique_lock<std::mutex>")
                continue;
            w.v.refuse("(i) " + where + " uses the wait's lock otherwise than to build, destroy and wait with it");
            return;
        }
        // std::condition_variable::wait<Pred>: `while (!__p()) wait(__lock);`
        unsigned waits = 0, preds = 0, returns = 0;
        bool other = false;
        w.waitBody.walk([&](Operation *op) {
            if (auto c = dyn_cast<cir::CallOp>(op)) {
                cir::FuncOp g = lookupCallee(c);
                if (g && contextOf(g) == "std::condition_variable" && baseOf(g) == "wait" && c.getNumOperands() == 2) ++waits;
                else if (g && !g.getBody().empty() && c.getNumOperands() == 1) {
                    ++preds;
                    w.pred = g;
                } else other = true;
                return;
            }
            if (isa<cir::ReturnOp>(op)) ++returns;
            else if (!isa<cir::AllocaOp, cir::LoadOp, cir::StoreOp, cir::WhileOp, cir::ConditionOp, cir::NotOp,
                          cir::ScopeOp, cir::YieldOp, cir::FuncOp, cir::CastOp, cir::GetMemberOp>(op))
                other = true;
        });
        if (waits != 1 || preds != 1 || returns != 1 || other)
            w.v.refuse("(h) " + shortName(w.waitBody) + " is not the predicate loop the pass reads");
    }

    // What each member of the predicate's closure holds, from the stores
    // that build it in the waiting function.
    llvm::DenseMap<unsigned, std::pair<Capture, Value>> captures;

    // (j)
    void predicate(Wait &w) {
        const std::string where = shortName(w.waitFn);
        // The closure handed to the wait: a copy (byval) of a local the
        // waiting function builds member by member.
        // A closure of up to 16 bytes is passed in registers: its bytes,
        // loaded as one or two `void *` from a copy of it.
        Value closure;
        for (unsigned i = 2; i < w.waitCall.getNumOperands(); ++i) {
            Value arg = w.waitCall.getOperand(i);
            if (auto ld = arg.getDefiningOp<cir::LoadOp>()) arg = ld.getAddr();
            arg = baseOfAddress(traceToLocalRoot(arg));
            Value c;
            for (OpOperand &use : arg.getUses())
                if (auto st = dyn_cast<cir::StoreOp>(use.getOwner()); st && st.getAddr() == arg)
                    if (auto ld = st.getValue().getDefiningOp<cir::LoadOp>()) c = traceToLocalRoot(ld.getAddr());
            if (!c) c = arg;
            if (closure && c != closure) closure = Value();
            else if (i == 2) closure = c;
        }
        if (!closure || !closure.getDefiningOp<cir::AllocaOp>()) {
            w.v.refuse("(j) " + where + " hands the wait a predicate the pass cannot read");
            return;
        }
        captures.clear();
        for (OpOperand &use : closure.getUses()) {
            auto gm = dyn_cast<cir::GetMemberOp>(use.getOwner());
            if (!gm) continue;
            for (OpOperand &u2 : gm.getResult().getUses()) {
                auto st = dyn_cast<cir::StoreOp>(u2.getOwner());
                if (!st || st.getAddr() != gm.getResult()) continue;
                Value v = st.getValue();
                auto pt = dyn_cast<cir::PointerType>(v.getType());
                Capture k;
                if (pt && pt.getPointee() == Type(w.rec)) k = Capture::This;
                else if (v.getDefiningOp<cir::AllocaOp>()) k = Capture::Local;
                else if (pt && recordName(pt.getPointee()) == "std::atomic<bool>") k = Capture::AtomicRef;
                else {
                    w.v.refuse("(j) the predicate of " + where + " captures " + gm.getName() + ", which the pass cannot follow");
                    return;
                }
                captures[gm.getIndex()] = {k, v};
            }
        }
        // The locals it captures by reference: reachable by nothing else.
        for (auto &[i, c] : captures) {
            if (c.first != Capture::Local) continue;
            for (OpOperand &use : c.second.getUses()) {
                Operation *u = use.getOwner();
                if (isa<cir::LoadOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
                if (auto st = dyn_cast<cir::StoreOp>(u)) {
                    if (st.getAddr() == c.second) continue;
                    if (auto gm = st.getAddr().getDefiningOp<cir::GetMemberOp>(); gm && gm.getAddr() == closure) continue;
                }
                if (auto call = dyn_cast<cir::CallOp>(u); call && queueOf(lookupCallee(call))) continue;
                w.v.refuse("(j) the predicate of " + where + " writes a local whose address other code may reach");
                return;
            }
        }
        // Its body.
        cir::FuncOp p = w.pred;
        Value self = p.getBody().front().getArgument(0);
        auto field = [&](Value addr) -> std::optional<unsigned> {
            auto gm = addr.getDefiningOp<cir::GetMemberOp>();
            if (!gm) return std::nullopt;
            Value b = gm.getAddr();
            if (auto ld = b.getDefiningOp<cir::LoadOp>()) b = ld.getAddr();
            if (auto a = b.getDefiningOp<cir::AllocaOp>()) {
                for (OpOperand &u : a.getResult().getUses())
                    if (auto st = dyn_cast<cir::StoreOp>(u.getOwner()); st && st.getValue() == self) return gm.getIndex();
            }
            if (b == self) return gm.getIndex();
            return std::nullopt;
        };
        // A value loaded from a closure member: which.
        auto loadedCapture = [&](Value v) -> std::optional<std::pair<Capture, unsigned>> {
            auto ld = v.getDefiningOp<cir::LoadOp>();
            if (!ld) return std::nullopt;
            auto i = field(ld.getAddr());
            if (!i || !captures.count(*i)) return std::nullopt;
            return std::make_pair(captures[*i].first, *i);
        };
        auto memberOfThis = [&](Value addr) -> std::optional<unsigned> {
            while (auto b = addr.getDefiningOp<cir::BaseClassAddrOp>()) addr = b.getOperand();
            auto gm = addr.getDefiningOp<cir::GetMemberOp>();
            if (!gm) return std::nullopt;
            auto c = loadedCapture(gm.getAddr());
            if (!c || c->first != Capture::This) return std::nullopt;
            return gm.getIndex();
        };
        std::string why;
        p.walk([&](Operation *op) {
            if (!why.empty()) return;
            if (isa<cir::AllocaOp, cir::ConstantOp, cir::NotOp, cir::TernaryOp, cir::SelectOp, cir::YieldOp,
                    cir::ScopeOp, cir::IfOp, cir::ReturnOp, cir::CmpOp, cir::GetMemberOp, cir::BaseClassAddrOp,
                    cir::FuncOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(op))
                return;
            if (auto c = dyn_cast<cir::CastOp>(op); c && !isa<cir::PointerType>(c.getResult().getType())) return;
            if (auto ld = dyn_cast<cir::LoadOp>(op)) {
                Value a = ld.getAddr();
                if (a.getDefiningOp<cir::AllocaOp>() || field(a)) return;
                if (auto c = loadedCapture(a); c && c->first == Capture::Local) return;
                why = "reads memory the pass cannot place without the mutex";
                return;
            }
            if (auto st = dyn_cast<cir::StoreOp>(op)) {
                Value a = st.getAddr();
                if (a.getDefiningOp<cir::AllocaOp>()) return;
                if (auto c = loadedCapture(a); c && c->first == Capture::Local) return;
                why = "writes memory other than the waiting function's locals";
                return;
            }
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                cir::FuncOp g = lookupCallee(call);
                if (!g) {
                    why = "makes an indirect call";
                    return;
                }
                const std::string ctx = contextOf(g), base = squashSpaces(baseOf(g));
                if (atomicBoolType(ctx) && atomicRead(base) && call.getNumOperands() >= 1) {
                    if (auto m = memberOfThis(call.getOperand(0))) {
                        w.predAtomics.insert(*m);
                        return;
                    }
                    if (auto c = loadedCapture(call.getOperand(0)); c && c->first == Capture::AtomicRef) {
                        // Which atomic: a member, through the reference the
                        // waiting function captured, or of a type only.
                        Value v = captures[c->second].second;
                        if (auto ld = v.getDefiningOp<cir::LoadOp>())
                            if (auto slot = ld.getAddr().getDefiningOp<cir::AllocaOp>()) {
                                Value only;
                                unsigned stores = 0;
                                for (OpOperand &u : slot.getResult().getUses())
                                    if (auto st = dyn_cast<cir::StoreOp>(u.getOwner()); st && st.getAddr() == slot.getResult()) {
                                        only = st.getValue();
                                        ++stores;
                                    }
                                if (stores == 1) v = only;
                            }
                        if (auto m = memberOf(v, w.rec)) w.predAtomics.insert(*m);
                        else if (auto pt = dyn_cast<cir::PointerType>(v.getType())) w.predAtomicTypes.push_back(pt.getPointee());
                        return;
                    }
                }
                if (const QueueClassForWaits *q = queueOf(g); q && q->tryPops.contains(g) && call.getNumOperands() == 2) {
                    auto m = memberOfThis(call.getOperand(0));
                    auto out = loadedCapture(call.getOperand(1));
                    if (m && w.rec.getElementType(*m) == Type(q->rec) && out && out->first == Capture::Local) {
                        w.popped = q;
                        return;
                    }
                }
                why = "calls " + shortName(g);
                return;
            }
            why = "does " + op->getName().getStringRef().str();
        });
        if (!why.empty()) w.v.refuse("(j) the predicate of " + where + " " + why);
    }

    // The section a lock object's constructor opens: the body of the
    // cleanup scope that follows it and destroys the lock.
    static cir::CleanupScopeOp sectionOf(cir::CallOp lockCtor) {
        Value slot = traceToLocalRoot(lockCtor.getOperand(0));
        Operation *next = lockCtor->getNextNode();
        auto cl = dyn_cast_or_null<cir::CleanupScopeOp>(next);
        if (!cl || cl.getCleanupRegion().empty()) return {};
        bool destroys = false;
        cl.getCleanupRegion().walk([&](cir::CallOp c) {
            if (c.getNumOperands() == 1 && traceToLocalRoot(c.getOperand(0)) == slot && nameOf(lookupCallee(c).getSymName()).dtor)
                destroys = true;
        });
        return destroys ? cl : cir::CleanupScopeOp();
    }

    static bool atomicRmw(StringRef base) {
        for (const char *x : {"operator++", "operator--", "fetch_add", "fetch_sub", "fetch_and", "fetch_or",
                              "fetch_xor", "operator+=", "operator-=", "operator&=", "operator|=", "operator^=",
                              "exchange", "compare_exchange_weak", "compare_exchange_strong"})
            if (base == x) return true;
        return false;
    }

    // (i): a lock object of the mutex is built, destroyed, and, for the
    // wait's, waited with; release(), unlock(), mutex() and the rest would
    // take the mutex past the section the pass reads, or out of it.
    void lockUses(Wait &w) {
        for (cir::CallOp l : w.locks) {
            Value slot = traceToLocalRoot(l.getOperand(0));
            const std::string at = shortName(l->getParentOfType<cir::FuncOp>());
            if (!slot.getDefiningOp<cir::AllocaOp>()) {
                w.v.refuse("(i) " + at + " builds a lock of " + w.mutexName + " the pass cannot place");
                continue;
            }
            for (OpOperand &use : slot.getUses()) {
                Operation *u = use.getOwner();
                if (u == l.getOperation() || isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
                if (l == w.waitLock && u == w.waitCall.getOperation()) continue;
                if (auto c = dyn_cast<cir::CallOp>(u); c && c.getNumOperands() == 1 && lookupCallee(c) &&
                                                       nameOf(lookupCallee(c).getSymName()).dtor)
                    continue;
                w.v.refuse("(i) " + at + " uses a lock of " + w.mutexName +
                           " otherwise than to build and destroy it" + (l == w.waitLock ? " and wait with it" : ""));
                break;
            }
        }
    }

    // (k)
    void sections(Wait &w) {
        const std::string where = shortName(w.waitFn);
        lockUses(w);
        if (w.v.refused()) return;
        cir::CleanupScopeOp ws = sectionOf(w.waitLock);
        if (!ws || !ws->isAncestor(w.waitCall)) {
            w.v.refuse("(k) the section of " + where + " that waits cannot be read");
            return;
        }
        // The wait's section runs without the mutex once lifted: the other
        // waiters run theirs at the same time, and the sections that keep
        // the mutex no longer exclude it. It reads and writes only its
        // locals, and changes the class's state by at most one atomic
        // read-modify-write on each side of the wait (the original's
        // mutex made each side one step), of a member the predicate does
        // not read and no other section touches.
        std::string why;
        unsigned before = 0, after = 0;
        Operation *waitOp = w.waitCall.getOperation();
        bool passed = false;  // the walk visits the section's operations in their order
        ws.getBodyRegion().walk([&](Operation *op) {
            if (op == waitOp) passed = true;
            if (!why.empty() || op == waitOp) return;
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                cir::FuncOp g = lookupCallee(call);
                const std::string ctx = contextOf(g);
                if (g && atomicType(ctx) && atomicRmw(squashSpaces(baseOf(g))) && call.getNumOperands() >= 1) {
                    auto m = memberOf(call.getOperand(0), w.rec);
                    auto pt = dyn_cast<cir::PointerType>(call.getOperand(0).getType());
                    const bool aliased = pt && llvm::any_of(w.predAtomicTypes, [&](Type t) {
                        return holdsByValue(t, pt.getPointee()) || holdsByValue(pt.getPointee(), t);
                    });
                    if (!m) {
                        why = "changes an atomic that is not a member of " + recordName(w.rec).str();
                    } else if (w.predAtomics.contains(*m) || aliased) {
                        why = "changes " + recordName(w.rec).str() + "'s member the predicate reads";
                    } else if (llvm::any_of(call->getResults(), [](Value r) { return !r.use_empty(); })) {
                        why = "uses what an atomic read-modify-write returns";
                    } else {
                        unsigned &n = passed ? after : before;
                        if (++n > 1) why = "changes the class's state in more than one step on one side of the wait";
                        w.waitWrites.insert(*m);
                    }
                    return;
                }
                why = "calls " + (g ? shortName(g) : std::string("a function through a pointer"));
                return;
            }
            if (auto ld = dyn_cast<cir::LoadOp>(op)) {
                if (!baseOfAddress(ld.getAddr()).getDefiningOp<cir::AllocaOp>()) why = "reads memory other than its locals";
                return;
            }
            if (auto st = dyn_cast<cir::StoreOp>(op)) {
                if (!baseOfAddress(st.getAddr()).getDefiningOp<cir::AllocaOp>()) why = "writes memory other than its locals";
                return;
            }
            if (auto cp = dyn_cast<cir::CopyOp>(op)) {
                if (!baseOfAddress(cp.getDst()).getDefiningOp<cir::AllocaOp>() ||
                    !baseOfAddress(cp.getSrc()).getDefiningOp<cir::AllocaOp>())
                    why = "copies memory other than its locals";
                return;
            }
            if (isa<cir::AtomicFetchOp, cir::AtomicXchgOp, cir::AtomicCmpXchgOp, cir::MemCpyOp, cir::MemMoveOp,
                    cir::MemSetOp, cir::InlineAsmOp, cir::ThrowOp, cir::CIRCallOpInterface>(op))
                why = "does " + op->getName().getStringRef().str();
        });
        if (!why.empty()) {
            w.v.refuse("(k) the section of " + where + " that waits " + why + ", and runs without " + w.mutexName +
                       " once lifted");
            return;
        }
        // What the waiting section changes is reached only by the module's
        // code: its address goes only to its atomic operations.
        module.walk([&](cir::GetMemberOp gm) {
            auto m = memberOf(gm.getResult(), w.rec);
            if (!m || !w.waitWrites.contains(*m) || w.v.refused()) return;
            std::function<bool(Value)> atomicOnly = [&](Value v) {
                for (OpOperand &u : v.getUses()) {
                    if (auto b = dyn_cast<cir::BaseClassAddrOp>(u.getOwner()); b && atomicOnly(b.getResult())) continue;
                    auto c = dyn_cast<cir::CallOp>(u.getOwner());
                    if (c && u.getOperandNumber() == 0 && lookupCallee(c) && atomicType(contextOf(lookupCallee(c)))) continue;
                    return false;
                }
                return true;
            };
            if (!atomicOnly(gm.getResult()))
                w.v.refuse("(k) " + shortName(gm->getParentOfType<cir::FuncOp>()) + " takes the address of " +
                           gm.getName() + ", which the waiting section changes, past its atomic operations");
        });
        if (w.v.refused()) return;
        // Every other section keeps the mutex and must change nothing the
        // predicate reads, and touch nothing the wait's section changes.
        llvm::DenseSet<Operation *> notifies;
        for (cir::CallOp n : llvm::concat<cir::CallOp>(w.notifyOne, w.notifyAll)) notifies.insert(n);
        Reach changesPred = reach([&](cir::FuncOp f) -> std::optional<std::string> {
            if (queueOf(f)) return "calls " + shortName(f);
            if (atomicBoolType(contextOf(f)) && !atomicRead(squashSpaces(baseOf(f))))
                return "writes a std::atomic<bool> (" + shortName(f) + ")";
            return std::nullopt;
        }, /*strict=*/true);
        // The waiting section's own touches are the change itself; a call
        // back into it from a section of the mutex would wait for the mutex
        // the original holds there.
        auto touchesWaitState = [&](Operation *root) -> std::optional<std::string> {
            std::optional<std::string> out;
            root->walk([&](cir::GetMemberOp gm) {
                if (ws->isAncestor(gm)) return;
                auto m = memberOf(gm.getResult(), w.rec);
                if (!out && m && w.waitWrites.contains(*m))
                    out = "touches " + gm.getName().str() + ", which the waiting section changes";
            });
            return out;
        };
        Reach touches = reach([&](cir::FuncOp f) { return touchesWaitState(f); }, /*strict=*/false);
        for (cir::CallOp l : w.locks) {
            if (l == w.waitLock) continue;
            const std::string at = shortName(l->getParentOfType<cir::FuncOp>());
            cir::CleanupScopeOp s = sectionOf(l);
            if (!s) {
                w.v.refuse("(k) the section of " + w.mutexName + " in " + at + " cannot be read");
                continue;
            }
            std::string r = reaches(s.getBodyRegion(), changesPred, notifies, true);
            if (!r.empty()) {
                w.v.refuse("(k) the section of " + w.mutexName + " in " + at + " " + r +
                           ", which may change what the predicate reads");
                continue;
            }
            if (auto t = touchesWaitState(s)) r = *t;
            else r = reaches(s.getBodyRegion(), touches, notifies, false);
            if (!r.empty())
                w.v.refuse("(k) the section of " + w.mutexName + " in " + at + " " + r +
                           ", which it no longer excludes once lifted");
        }
        // A notification decided on what the waiting section changes (a
        // count of the waiters read under the mutex): lifted, the waiter
        // announces itself with no mutex between that write and its look,
        // and the decision may miss it. The eventcount's notify reads its
        // own count after its fence; one the program reads may be stale.
        llvm::DenseSet<Operation *> seen;
        for (Operation *n : notifies) {
            auto f = n->getParentOfType<cir::FuncOp>();
            if (!seen.insert(f).second) continue;
            std::optional<std::string> t = touchesWaitState(f);
            std::string r = t ? *t : reaches(f.getBody(), touches, notifies, false);
            if (!r.empty())
                w.v.refuse("(k) " + shortName(f) + ", which notifies " + w.cvName + ", " + r +
                           ": a notification decided on it may miss a waiter once lifted");
        }
    }

    // The module's calls, for the questions condition (k) asks of a
    // section: whether it may, at any depth, change what the predicate
    // reads, or touch what the wait's section changes. Built once.
    //
    // A direct call reaches its callee. A call through a pointer reaches
    // every function whose address the module takes (a vtable's entry, a
    // function made a pointer, a pointer to member) that it may legally
    // call: one whose parameters its arguments fit (the same type; a
    // pointer to a class the parameter's class derives from, whose
    // overrider a virtual call reaches; anything, for an argument the front
    // end passes as `void *`, as the object of a call through a pointer to
    // member), as many as its parameters, or at least as many for a
    // variadic function. A call through a pointer that reaches none of
    // them may reach anything. A function with no body here reaches what
    // its model says (classifyExternal): the program's replacements of
    // operator new and delete, and, for one that may destroy a thrown
    // object, every destructor whose address the module takes (as every
    // throw and std::make_exception_ptr does); with the program's own
    // new-handler or terminate handler installed, anything.
    bool graphDone = false;
    llvm::SmallVector<cir::FuncOp, 32> taken;  // the functions whose address the module takes
    llvm::DenseMap<Operation *, llvm::SmallVector<Operation *, 4>> callers;
    // Functions that may do anything: an unknown function with no body
    // here, a call through a pointer that resolves to nothing, an
    // operation that calls the pass cannot follow.
    llvm::DenseMap<Operation *, std::string> anything;
    // The program's replacements of operator new and delete.
    llvm::SmallVector<Operation *, 4> allocators;
    // The calls back into the module an unknown function with no body here
    // may make: every function whose address is taken.
    llvm::DenseMap<Operation *, llvm::SmallVector<Operation *, 4>> callbacks;

    // A call through a pointer on an object of a class of libstdc++'s (its
    // first argument), whose overriders are libstdc++'s when none of the
    // module's fits.
    static bool libraryObject(cir::CallOp call) {
        if (call.getArgOperands().empty()) return false;
        auto pt = dyn_cast<cir::PointerType>(call.getArgOperands().front().getType());
        StringRef n = pt ? recordName(pt.getPointee()) : StringRef();
        return n.starts_with("std::") || n.starts_with("__gnu_cxx::");
    }

    static bool fits(Type param, Type arg) {
        if (param == arg) return true;
        auto pp = dyn_cast<cir::PointerType>(param), pa = dyn_cast<cir::PointerType>(arg);
        if (!pp || !pa) return false;
        if (isa<cir::VoidType>(pa.getPointee())) return true;
        auto rp = dyn_cast<cir::RecordType>(pp.getPointee()), ra = dyn_cast<cir::RecordType>(pa.getPointee());
        // A class may derive from another when it holds it by value (a base
        // subobject is held; so, over-approximating, is a member).
        return rp && ra && holdsByValue(rp, ra);
    }
    llvm::SmallVector<cir::FuncOp, 4> targets(cir::CallOp call) {
        llvm::SmallVector<cir::FuncOp, 4> out;
        llvm::SmallVector<Type, 4> args;
        for (Value v : call.getArgOperands()) args.push_back(v.getType());
        bool callVariadic = false;
        if (Value c = call.getIndirectCall())
            if (auto pt = dyn_cast<cir::PointerType>(c.getType()))
                if (auto ft = dyn_cast<cir::FuncType>(pt.getPointee())) callVariadic = ft.isVarArg();
        for (cir::FuncOp f : taken) {
            llvm::ArrayRef<Type> ps = f.getArgumentTypes();
            const bool variadic = f.getFunctionType().isVarArg() || callVariadic;
            if (variadic ? args.size() < ps.size() : args.size() != ps.size()) continue;
            bool ok = true;
            for (unsigned i = 0; i < ps.size() && ok; ++i) ok = fits(ps[i], args[i]);
            if (ok) out.push_back(f);
        }
        return out;
    }
    void buildGraph() {
        graphDone = true;
        llvm::SetVector<Operation *> takenSet;
        bool handlers = false;
        module.walk([&](Operation *op) {
            if (auto gg = dyn_cast<cir::GetGlobalOp>(op))
                if (auto f = module.lookupSymbol<cir::FuncOp>(gg.getName())) takenSet.insert(f);
            if (auto c = dyn_cast<cir::CallOp>(op))
                if (cir::FuncOp g = lookupCallee(c)) {
                    Name n = nameOf(g.getSymName());
                    if (n.ok && n.context == "std" &&
                        (n.base == "set_terminate" || n.base == "set_new_handler" || n.base == "set_unexpected"))
                        handlers = true;
                }
            if (isa<cir::FuncOp>(op)) return;  // an alias names its aliasee, which takes no address
            for (NamedAttribute a : op->getAttrs())
                a.getValue().walk([&](cir::GlobalViewAttr gv) {
                    if (auto f = module.lookupSymbol<cir::FuncOp>(gv.getSymbol().getValue())) takenSet.insert(f);
                });
        });
        taken.clear();
        for (Operation *op : takenSet) taken.push_back(cast<cir::FuncOp>(op));
        llvm::SmallVector<Operation *, 4> destructors;
        allocators.clear();
        for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
            Name n = nameOf(f.getSymName());
            if (!f.getBody().empty() && n.ok && n.context.empty() && StringRef(n.base).starts_with("operator new"))
                allocators.push_back(f);
            if (!f.getBody().empty() && n.ok && n.context.empty() && StringRef(n.base).starts_with("operator delete"))
                allocators.push_back(f);
        }
        // The destructors of what the program throws: the third argument
        // of __cxa_throw and of __cxa_init_primary_exception (which
        // std::make_exception_ptr calls).
        module.walk([&](cir::CallOp c) {
            cir::FuncOp g = lookupCallee(c);
            if (!g || (g.getSymName() != "__cxa_throw" && g.getSymName() != "__cxa_init_primary_exception") ||
                c.getNumOperands() < 3)
                return;
            Value d = c.getOperand(2);
            while (auto cast = d.getDefiningOp<cir::CastOp>()) d = cast.getSrc();
            if (auto gg = d.getDefiningOp<cir::GetGlobalOp>()) {
                if (auto f = module.lookupSymbol<cir::FuncOp>(gg.getName())) destructors.push_back(f);
            } else if (!d.getDefiningOp<cir::ConstantOp>()) {
                anything.try_emplace(c->getParentOfType<cir::FuncOp>(),
                                     "throws an object whose destructor the pass cannot name");
            }
        });
        for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
            if (f.getBody().empty()) {
                if (atomicBoolType(contextOf(f))) continue;  // read or written, as its name says
                if (contextOf(f) == "std::condition_variable") continue;  // another wait's; (h) reads this one's
                switch (classifyExternal(f)) {
                case External::Pure: break;
                case External::Exceptional:
                    for (Operation *d : destructors) callers[d].push_back(f);
                    [[fallthrough]];
                case External::Allocating:
                    for (Operation *a : allocators) callers[a].push_back(f);
                    if (handlers) anything.try_emplace(f, "calls " + shortName(f) + ", which may run a handler the program installed");
                    break;
                case External::Unknown:
                    anything.try_emplace(f, "calls " + shortName(f) + ", which has no body here");
                    // What it may reach of the module's: the functions whose
                    // address the module takes, called back.
                    for (cir::FuncOp t : taken) callbacks[t].push_back(f);
                    break;
                }
                continue;
            }
            f.walk([&](cir::CIRCallOpInterface ci) {
                auto call = dyn_cast<cir::CallOp>(ci.getOperation());
                if (!call) {
                    anything.try_emplace(f, "makes a call the pass cannot follow (in " + shortName(f) + ")");
                    return;
                }
                if (cir::FuncOp g = lookupCallee(call)) {
                    callers[g].push_back(f);
                    return;
                }
                llvm::SmallVector<cir::FuncOp, 4> ts = targets(call);
                if (ts.empty()) {
                    // No function of the module's fits: the call reaches
                    // code the pass does not see. On an object of
                    // libstdc++'s, its own overriders, which may allocate.
                    if (libraryObject(call)) {
                        for (Operation *a : allocators) callers[a].push_back(f);
                        if (handlers) anything.try_emplace(f, "calls through a pointer to libstdc++, which may run a handler the program installed");
                    } else {
                        anything.try_emplace(f, "calls through a pointer the pass cannot resolve (in " + shortName(f) + ")");
                    }
                }
                for (cir::FuncOp g : ts) callers[g].push_back(f);
            });
        }
    }

    // The functions that may, at any depth, do what `base` says of a
    // function by itself, with the first reason each has, and the callee
    // that gave it.
    struct Reach {
        llvm::DenseMap<Operation *, std::string> why;
        llvm::DenseMap<Operation *, Operation *> via;
        llvm::DenseSet<Operation *> named;  // reached from the base itself
        std::string chainList(Operation *f) const {
            std::string out;
            for (unsigned i = 0; i < 6 && via.count(f); ++i) {
                f = via.lookup(f);
                out += ", " + shortName(cast<cir::FuncOp>(f));
            }
            return out;
        }
        std::string chain(Operation *f) const {
            std::string out = chainList(f);
            return out.empty() ? out : " (through " + out.substr(2) + ")";
        }
    };
    // `strict`: what the code the pass does not see may do counts as
    // anything (a change of what the predicate reads may be made through a
    // pointer it is handed); otherwise it counts as calling back every
    // function whose address the module takes (a member whose address
    // never leaves its atomic operations, (k), is reached only by the
    // module's code).
    Reach reach(llvm::function_ref<std::optional<std::string>(cir::FuncOp)> base, bool strict) {
        if (!graphDone) buildGraph();
        Reach r;
        llvm::SmallVector<Operation *, 32> work;
        auto mark = [&](Operation *f, const std::string &why, Operation *from) {
            if (!r.why.try_emplace(f, why).second) return;
            if (from) r.via[f] = from;
            work.push_back(f);
        };
        auto propagate = [&] {
            while (!work.empty()) {
                Operation *g = work.pop_back_val();
                const std::string why = r.why.lookup(g);
                for (Operation *f : callers.lookup(g)) mark(f, why, g);
                if (!strict)
                    for (Operation *f : callbacks.lookup(g)) mark(f, why, g);
            }
        };
        // What the base says first, so that a remark names it where it can.
        for (cir::FuncOp f : module.getOps<cir::FuncOp>())
            if (auto why = base(f)) mark(f, *why, nullptr);
        propagate();
        for (auto &[f, why] : r.why) r.named.insert(f);
        if (strict)
            for (auto &[f, why] : anything) mark(f, why, nullptr);
        propagate();
        return r;
    }

    // Why the calls of a region may, at any depth, do what `r` says;
    // empty when they cannot.
    std::string reaches(Region &region, const Reach &r, const llvm::DenseSet<Operation *> &skip, bool strict) {
        std::string why;
        region.walk([&](cir::CIRCallOpInterface ci) {
            if (!why.empty() || skip.contains(ci.getOperation())) return;
            auto call = dyn_cast<cir::CallOp>(ci.getOperation());
            if (!call) {
                why = "makes a call the pass cannot follow";
                return;
            }
            if (cir::FuncOp g = lookupCallee(call)) {
                auto it = r.why.find(g);
                if (it != r.why.end()) why = it->second + r.chain(g);
                return;
            }
            llvm::SmallVector<cir::FuncOp, 4> ts = targets(call);
            if (ts.empty() && strict && !libraryObject(call)) why = "calls through a pointer the pass cannot resolve";
            if (ts.empty() && strict && libraryObject(call))
                for (Operation *a : allocators)
                    if (r.why.count(a) && why.empty()) why = r.why.lookup(a) + " (through libstdc++'s allocation)";
            for (bool named : {true, false})
                for (cir::FuncOp g : ts) {
                    auto it = r.why.find(g);
                    if (it != r.why.end() && why.empty() && (!named || r.named.contains(g)))
                        why = it->second + " (through a pointer to " + shortName(g) + r.chainList(g) + ")";
                }
        });
        return why;
    }

    llvm::StringMap<cir::FuncOp> runtimeFor(cir::RecordType rec) {
        llvm::StringMap<cir::FuncOp> out;
        const std::string want = squashSpaces(("wtc::lifted_wait<" + recordName(rec) + ">").str());
        for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
            auto ann = f.getAnnotationsAttr();
            if (!ann || f.getBody().empty() || contextOf(f) != want) continue;
            for (Attribute a : ann) {
                auto an = dyn_cast<cir::AnnotationAttr>(a);
                if (!an) continue;
                StringRef n = an.getName().getValue();
                if (n.consume_front("wtc_lifted_wait_") || n.consume_front("wtc_lifted_")) out[n] = f;
            }
        }
        for (StringRef need : {"construct", "destroy", "defer", "begin", "step", "end", "notify_one", "notify_all"})
            if (!out.count(need)) return {};
        return out;
    }

    void lift(Wait &w, llvm::StringMap<cir::FuncOp> &rt) {
        MLIRContext *ctx = module.getContext();
        auto call = [&](OpBuilder &b, Location loc, StringRef which, ValueRange args) {
            cir::FuncOp f = rt.lookup(which);
            cir::CallOp::create(b, loc, FlatSymbolRefAttr::get(f.getSymNameAttr()), Type(), args);
        };
        cir::FuncOp ctor = rt.lookup("construct");
        llvm::ArrayRef<Type> ct = ctor.getArgumentTypes();
        for (cir::CallOp op : w.cvCtors) {
            OpBuilder b(op);
            auto i0 = cir::ConstantOp::create(b, op.getLoc(), ct[1], cir::IntAttr::get(ct[1], w.producers));
            Value waiters = cir::ConstantOp::create(b, op.getLoc(), ct[2], cir::IntAttr::get(ct[2], w.waiters));
            if (w.waitersSlot) {
                Value n = cir::LoadOp::create(b, op.getLoc(), w.waitersSlot.getResult());
                waiters = n.getType() == ct[2] ? n : cir::CastOp::create(b, op.getLoc(), ct[2], cir::CastKind::integral, n).getResult();
            }
            // A paced wait parks after its pause whatever the counts
            // (shim/lifted.hpp, lifted_wait_policy): what a waiter with a
            // requirement needs too.
            auto paced = cir::ConstantOp::create(b, op.getLoc(), ct[3],
                                                 cir::BoolAttr::get(ctx, cast<cir::BoolType>(ct[3]), w.paced || w.noSpin));
            call(b, op.getLoc(), "construct", {op.getOperand(0), i0.getResult(), waiters, paced.getResult()});
            op.erase();
        }
        for (cir::CallOp op : w.cvDtors) {
            OpBuilder b(op);
            call(b, op.getLoc(), "destroy", {op.getOperand(0)});
            op.erase();
        }
        for (cir::CallOp op : w.notifyOne) {
            OpBuilder b(op);
            call(b, op.getLoc(), "notify_one", {op.getOperand(0)});
            op.erase();
        }
        for (cir::CallOp op : w.notifyAll) {
            OpBuilder b(op);
            call(b, op.getLoc(), "notify_all", {op.getOperand(0)});
            op.erase();
        }
        {
            OpBuilder b(w.waitLock);
            call(b, w.waitLock.getLoc(), "defer", {w.waitLock.getOperand(0), w.waitLock.getOperand(1)});
            w.waitLock.erase();
        }
        // The wait: std::condition_variable::wait<Pred>'s loop, copied, its
        // sleep the runtime's step between begin and end.
        IRMapping map;
        auto loop = cast<cir::FuncOp>(w.waitBody->clone(map));
        loop.setSymName(("wtc.lifted_wait." + std::to_string(clones++)).c_str());
        SymbolTable(module).insert(loop, std::next(Block::iterator(w.waitBody.getOperation())));
        loop.setLinkage(cir::GlobalLinkageKind::InternalLinkage);
        loop.setComdat(false);
        loop->removeAttr("func_info");
        loop->removeAttr("sym_visibility");
        Block &entry = loop.getBody().front();
        Value cv = entry.getArgument(0);
        auto stateTy = cast<cir::PointerType>(rt.lookup("begin").getArgumentTypes()[1]);
        OpBuilder b = OpBuilder::atBlockBegin(&entry);
        auto st = cir::AllocaOp::create(b, loop.getLoc(), stateTy, "wait.state", b.getI64IntegerAttr(8));
        cir::CallOp inner;
        cir::ReturnOp ret;
        loop.walk([&](Operation *op) {
            if (auto c = dyn_cast<cir::CallOp>(op); c && contextOf(lookupCallee(c)) == "std::condition_variable") inner = c;
            if (auto r = dyn_cast<cir::ReturnOp>(op)) ret = r;
        });
        // Just before the loop: the operation of the entry block that holds it.
        Operation *outer = inner;
        while (outer->getBlock() != &entry) outer = outer->getParentOp();
        b.setInsertionPoint(outer);
        call(b, loop.getLoc(), "begin", {cv, st.getResult()});
        {
            OpBuilder ib(inner);
            call(ib, inner.getLoc(), "step", {inner.getOperand(0), st.getResult()});
            inner.erase();
        }
        {
            OpBuilder rb(ret);
            call(rb, ret.getLoc(), "end", {cv, st.getResult()});
        }
        {
            OpBuilder cb(w.waitCall);
            cir::CallOp::create(cb, w.waitCall.getLoc(), FlatSymbolRefAttr::get(loop.getSymNameAttr()), Type(),
                                w.waitCall.getOperands());
            w.waitCall.erase();
        }
        emitRemark(w.loc) << "wtc: locked wait " << w.v.label << " lifted (wait in " << shortName(w.waitFn) << ", "
                          << w.notifyOne.size() << " notify_one, " << w.notifyAll.size() << " notify_all; "
                          << w.ends << ")";
    }
};

}  // namespace

std::string mlir::wtc::liftWaits(ModuleOp module, const ClosedWorld &world, llvm::ArrayRef<QueueClassForWaits> queues,
                                 bool rewrite) {
    return Lifter(module, world, queues).run(rewrite);
}
