#include "WTC/Transforms/LiftedBounded.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace mlir;
using namespace mlir::wtc;

// Queues the program wrote itself. A class that wraps a standard container
// under a std::mutex, with condition variables to wait on, is a queue in
// all but name:
//
//     void push(T v)        { lock_guard l(m_); q_.push(move(v)); } cv_.notify_one();
//     void pop(T& out)      { unique_lock l(m_); cv_.wait(l, [&]{ return !q_.empty(); });
//                             out = move(q_.front()); q_.pop(); }
//
// This pass recognises such a class from its member functions in CIR, where
// the container's operations are still calls named by their mangled names,
// proves that substituting it changes nothing the program can rely on, and
// replaces the bodies of its constructor, destructor and recognised member
// functions with calls into wtc::lifted<C> (shim/lifted.hpp), whose push and
// pop the lifting pass then raises to wtc.queue_push and wtc.queue_pop like
// any shim queue's. The topology and ownership analyses and the
// specialisation pass apply unchanged.
//
// The proof is conservative and names the reason for every refusal:
//
//   (a) the container member is touched only by the constructor, the
//       destructor and member functions that each match one operation:
//       push, try-pop, blocking pop, closable blocking pop, close; and
//       those functions do nothing else the program could see, since the
//       rewrite replaces their bodies whole (a local object is built only
//       if it is a lock or a duration);
//   (b) every touch of it in those functions is with the class's mutex
//       held, all of one function's in one critical section;
//   (c) the mutex and the condition variables are used only by those
//       functions, and the mutex guards nothing else: any other member
//       touched inside a critical section refuses the class;
//   (d) every wait is the predicate loop on the condition variable,
//       written as a while loop or as wait(lock, predicate), its predicate
//       "not empty" or "not empty or stopped"; a timed wait, a wait outside
//       such a loop, a push that does not notify the variable a pop waits
//       on, and a pop that gives up at the stop flag with elements still
//       queued are refused;
//   (e) the order is FIFO: the container is a std::queue whose member is a
//       std::deque or a std::list, a std::deque or a std::list, elements
//       of its own type go in at the back and come out at the front;
//       anything else (back, pop_back, insert, erase, swap, clear, size, a
//       container of another kind, emplace from another type) refuses;
//   (f) the world is closed: the module has main, is not linked later with
//       files the pass does not see, none of the files linked with it
//       names the class, and every member function of the class has its
//       body here. A lifted member function treats the object's storage
//       differently, and another unit's copy of it, as written, would not.
//
// A stop flag must start false, by a constant, in every constructor: the
// substitute starts open.
//
// The predicates and the conditions around them are compared by truth
// table over the two facts they may read, "the container is empty" and
// "the stop flag is set", so that `!q.empty() || stop`, `stop ||
// !q.empty()` and `while (q.empty() && !stop)` are one predicate.
//
// A container that is not standard may still be a bounded ring the class
// wrote itself (spdlog's circular_q): LiftedBounded.cpp reads its member
// functions symbolically, and the class's pushes then also wait for room,
// decline or overrun the oldest, its pops may wait with a time limit, and
// its size and overrun count may be read. Such a class becomes
// wtc::lifted_bounded<C> over wtc::bounded, whose capacity is exact. An
// overrunning push makes its callers consumers too, unless no call of it
// can run by the values the overflow policy may take (ValueSets).
//
// The runtime is a template the program never instantiates. In the first
// of its two runs per file, wtc-cc runs this pass with `instantiations=`,
// which writes `template struct wtc::lifted<C>;` for each class that would
// lift, compiles the file again with those lines appended, and runs the
// pipeline on that. A class whose container type cannot be spelled at the
// end of the file (a lambda, an anonymous namespace) is refused.

namespace {

// A function's name in parts, from the Itanium demangler.
struct Name {
    std::string base, context, params;
    bool ctor = false, dtor = false, ok = false;
};

Name nameOf(StringRef mangled) {
    Name n;
    if (!mangled.starts_with("_Z")) {
        n.base = mangled.str();
        return n;
    }
    const std::string s = mangled.str();
    llvm::ItaniumPartialDemangler d;
    if (d.partialDemangle(s.c_str()) || !d.isFunction()) return n;
    auto take = [](char *c) {
        std::string out = c ? c : "";
        std::free(c);
        return out;
    };
    size_t len = 0;
    n.base = take(d.getFunctionBaseName(nullptr, &len));
    n.context = take(d.getFunctionDeclContextName(nullptr, &len));
    n.params = take(d.getFunctionParameters(nullptr, &len));
    n.ctor = d.isCtorOrDtor() && !StringRef(n.base).starts_with("~");
    n.dtor = d.isCtorOrDtor() && StringRef(n.base).starts_with("~");
    n.ok = true;
    return n;
}

std::string pretty(StringRef mangled) { return llvm::demangle(mangled.str()); }

StringRef recordName(Type t) {
    auto r = dyn_cast_or_null<cir::RecordType>(t);
    return r && r.getName() ? r.getName().getValue() : StringRef();
}

// Where a member function's name sits inside the demangled one, for remarks
// that must stay short: "shared_queue<int>::push".
std::string shortName(cir::FuncOp f) {
    Name n = nameOf(f.getSymName());
    if (!n.ok) return f.getSymName().str();
    return n.context.empty() ? n.base : n.context + "::" + n.base;
}

// Ring: a record the pass reads as a bounded FIFO ring (LiftedBounded.h).
enum class Kind { Unknown, StdQueue, StdDeque, StdList, StdVector, StdPriorityQueue, Ring, Other };

Kind containerKind(StringRef name) {
    if (name.starts_with("std::queue<")) return Kind::StdQueue;
    if (name.starts_with("std::deque<")) return Kind::StdDeque;
    if (name.starts_with("std::list<") || name.starts_with("std::__cxx11::list<")) return Kind::StdList;
    if (name.starts_with("std::vector<")) return Kind::StdVector;
    if (name.starts_with("std::priority_queue<")) return Kind::StdPriorityQueue;
    return Kind::Other;
}


bool isStopType(Type t) { return isa<cir::BoolType>(t) || recordName(t) == "std::atomic<bool>"; }

// Truth tables over (empty, stopped, full): bit 4*f + 2*e + s. A table
// that does not read "full" is the same in both halves; only a bounded
// ring answers full().
using Table = unsigned;
constexpr Table kAll = 0xFF;
constexpr Table bitOf(bool e, bool s, bool f) { return 1u << (4 * f + 2 * e + s); }
constexpr Table eitherFull(Table t) { return t | (t << 4); }
constexpr Table bitOf(bool e, bool s) { return eitherFull(bitOf(e, s, false)); }
constexpr Table kNotEmpty = bitOf(false, false) | bitOf(false, true);
constexpr Table kNotEmptyOrStop = kNotEmpty | bitOf(true, true);
constexpr Table kEmpty = bitOf(true, false) | bitOf(true, true);
// Not full, whatever else: the wait of a push into a bounded ring.
constexpr Table kNotFull = 0x0F;
// States in which a wait for "not empty or stopped" has ended.
constexpr Table kAfterClosableWait = kNotEmptyOrStop;

struct Stmt {
    // GuardedPush: `if (!full) { push; flag = true; }`; FlagIf, after the
    // critical section: `if (flag) notify; else <the program's own code>`;
    // TimedWait: `wait_for(lock, d, pred)`, then TimedOut: `if (!that)
    // return false;`; Getter: the ring's size or overrun count, returned;
    // Reset: the ring's overrun count set to zero.
    enum K { Push, Front, Out, PopFront, Wait, Guard, Notify, SetStop, Return, GuardedPush, FlagIf, TimedWait,
             TimedOut, Getter, Reset } k;
    explicit Stmt(K k) : k(k) {}
    Operation *op = nullptr;
    bool held = false;
    unsigned cs = 0;
    bool move = false;            // Push, Out
    unsigned param = 0;           // Push: the value's parameter; Out: the destination's
    bool byValueSlot = false;     // Push: a scalar parameter, its address the slot it was stored in
    Value front;                  // Front: the reference; Out: the reference read
    Table table = 0;              // Wait: when it ends; Guard: when it returns
    unsigned cv = 0;              // Wait, Notify
    bool all = false;             // Notify
    std::optional<bool> value;    // Guard, Return: the constant returned, none for void
    bool returnsSomething = false;
    bool fromGetter = false;      // Return: what a Getter read
    Value flag;                   // GuardedPush, FlagIf: the local slot of the flag
    Operation *section = nullptr; // GuardedPush: the scope that is the critical section
    unsigned durationParam = 0;   // TimedWait
    std::string unit;             // TimedWait: ns, us, ms or s
    RingOp getter = RingOp::None; // Getter
};

// PushWait, TryPush, PushOverrun, PopWaitFor and the counters exist only
// for a bounded ring.
enum class Role { None, Push, TryPop, PopWait, PopWaitClosed, Close, PushWait, TryPush, PushOverrun, PopWaitFor,
                  Size, OverrunCounter, ResetOverrun };

const char *roleName(Role r) {
    switch (r) {
    case Role::PushWait: return "push that waits while full";
    case Role::TryPush: return "push that discards when full";
    case Role::PushOverrun: return "push that overruns the oldest when full";
    case Role::PopWaitFor: return "blocking pop with a time limit";
    case Role::Size: return "size";
    case Role::OverrunCounter: return "overrun count";
    case Role::ResetOverrun: return "reset of the overrun count";
    case Role::Push: return "push";
    case Role::TryPop: return "try-pop";
    case Role::PopWait: return "blocking pop";
    case Role::PopWaitClosed: return "closable blocking pop";
    case Role::Close: return "close";
    case Role::None: break;
    }
    return "none";
}

struct Method {
    cir::FuncOp f;
    Role role = Role::None;
    bool move = true;
    unsigned param = 0;
    bool byValueSlot = false;
    std::optional<bool> pushReturns;  // Push: void, or this constant
    unsigned waitCv = 0;
    unsigned flag = 0;  // Close: the member it sets
    llvm::SmallVector<std::pair<unsigned, bool>, 2> notifies;  // cv, all
    // TryPush: the critical section to replace, the flag it sets, and the
    // notifications after it, which the runtime does.
    Operation *section = nullptr;
    Value pushedFlag;
    llvm::SmallVector<Operation *, 2> notifyOps;
    // PopWaitFor: the duration's parameter and unit.
    unsigned durationParam = 0;
    std::string unit;
};

struct Candidate {
    cir::RecordType rec;
    unsigned q = 0;
    std::optional<unsigned> mutex;
    llvm::SmallVector<unsigned, 2> cvs;
    std::optional<unsigned> stop;
    Kind kind = Kind::Unknown;
    Type elem;       // the element's CIR type, from the container's calls
    Type frontElem;  // the container's value_type, from what front() refers to
    bool emplaces = false;
    llvm::SmallVector<Method, 4> methods;
    llvm::SmallVector<cir::FuncOp, 2> ctors, dtors;
    llvm::SmallVector<cir::FuncOp, 2> preds;  // wait predicates read
    // What each constructor stores into each flag-typed member: the
    // constant, or none when it is computed or left indeterminate.
    llvm::DenseMap<Operation *, llvm::SmallVector<std::pair<unsigned, std::optional<bool>>, 2>> flagInits;
    llvm::SmallVector<std::string, 4> reasons;
    std::map<std::string, unsigned> features;
    Location loc;
    // A bounded ring: its reading, and whether a push may overrun.
    std::shared_ptr<RingModel> ring;
    bool overrunReachable = false;

    explicit Candidate(Location l) : loc(l) {}

    std::string label() const {
        return (memberName(q) + " in " + recordName(rec)).str();
    }
    StringRef memberName(unsigned i) const;
    void refuse(const Twine &why) {
        std::string s = why.str();
        if (llvm::find(reasons, s) == reasons.end()) reasons.push_back(s);
    }
};

// Member names are not part of a CIR record type; they are on the
// get_member operations, so the pass keeps the first it sees for each.
llvm::DenseMap<std::pair<Type, unsigned>, std::string> *gMemberNames = nullptr;

StringRef Candidate::memberName(unsigned i) const {
    auto it = gMemberNames->find({rec, i});
    return it == gMemberNames->end() ? StringRef("<member>") : StringRef(it->second);
}

// The function's `this`: its first argument when the function is a member
// function of `rec`, or, in a lambda's call operator, the `this` the lambda
// captured.
// The index of a member function's `this`: 0, or 1 after the hidden
// return slot of a function that returns an object.
unsigned thisIndex(cir::FuncOp f) {
    return f.getNumArguments() >= 2 && f.getArgAttr(0, "llvm.sret") ? 1 : 0;
}

bool isThis(Value v, cir::FuncOp f, cir::RecordType rec) {
    v = traceToRoot(v);
    if (auto arg = dyn_cast<BlockArgument>(v)) {
        if (arg.getOwner() != &f.getBody().front() || arg.getArgNumber() != thisIndex(f)) return false;
        auto pt = dyn_cast<cir::PointerType>(arg.getType());
        return pt && pt.getPointee() == rec;
    }
    // A lambda: load (get_member %closure[k] {name = "this"}), %closure its
    // own first argument.
    auto ld = v.getDefiningOp<cir::LoadOp>();
    if (!ld) return false;
    auto gm = traceToLocalRoot(ld.getAddr()).getDefiningOp<cir::GetMemberOp>();
    if (!gm || gm.getName() != "this") return false;
    auto pt = dyn_cast<cir::PointerType>(ld.getType());
    if (!pt || pt.getPointee() != rec) return false;
    auto base = dyn_cast<BlockArgument>(traceToRoot(gm.getAddr()));
    return base && base.getOwner() == &f.getBody().front() && base.getArgNumber() == 0;
}

// The member of `this` an address names, if it does.
std::optional<unsigned> memberOfThis(Value addr, cir::FuncOp f, cir::RecordType rec) {
    if (!addr) return std::nullopt;
    auto gm = traceToLocalRoot(addr).getDefiningOp<cir::GetMemberOp>();
    if (!gm) return std::nullopt;
    auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
    if (!pt || pt.getPointee() != rec) return std::nullopt;
    if (!isThis(gm.getAddr(), f, rec)) return std::nullopt;
    return static_cast<unsigned>(gm.getIndex());
}

// The stack slot an address is in, through member and element steps: a
// local object, or a member of one (a lambda's captures, a temporary).
cir::AllocaOp localSlot(Value addr) {
    for (unsigned depth = 0; addr && depth < 16; ++depth) {
        addr = traceToLocalRoot(addr);
        if (auto a = addr.getDefiningOp<cir::AllocaOp>()) return a;
        if (auto gm = addr.getDefiningOp<cir::GetMemberOp>()) {
            addr = gm.getAddr();
            continue;
        }
        return cir::AllocaOp();
    }
    return cir::AllocaOp();
}

// Whether a member function belongs to the record: its demangled scope is
// the record's name, spaces aside (the demangler writes "> >" and "T*",
// CIR's record names ">>" and "T *"). A lambda's call operator is scoped
// to the function it is written in, and a free function to a namespace.
std::string squashSpaces(StringRef x) {
    std::string out;
    for (char ch : x)
        if (ch != ' ') out += ch;
    return out;
}

bool isMemberOf(cir::FuncOp f, cir::RecordType rec) {
    Name n = nameOf(f.getSymName());
    return n.ok && squashSpaces(n.context) == squashSpaces(recordName(rec));
}

// Whether a demangled name, spaces removed, mentions the class: its name
// as a whole word, as the scope of a member ("LQ<int>::pending()") or in a
// parameter ("report(LQ<int>&)").
bool mentions(StringRef name, StringRef cls) {
    auto word = [](char ch) { return llvm::isAlnum(ch) || ch == '_'; };
    for (size_t at = name.find(cls); at != StringRef::npos; at = name.find(cls, at + 1)) {
        const size_t end = at + cls.size();
        if ((at == 0 || !word(name[at - 1])) && (end == name.size() || !word(name[end]))) return true;
    }
    return false;
}

// The local objects a lifted function may build and destroy: those whose
// constructors and destructors touch nothing but the object itself.
bool effectFreeLocal(StringRef type) {
    for (StringRef known : {"std::lock_guard<std::mutex>", "std::unique_lock<std::mutex>",
                            "std::scoped_lock<std::mutex>", "std::chrono::duration<", "std::chrono::time_point<"})
        if (known.ends_with("<") ? type.starts_with(known) : type == known) return true;
    return false;
}

// The parameter a value is, through the slot it was stored in at entry.
std::optional<unsigned> paramOf(Value v, cir::FuncOp f) {
    auto arg = dyn_cast<BlockArgument>(traceToRoot(v));
    if (!arg || arg.getOwner() != &f.getBody().front()) return std::nullopt;
    return arg.getArgNumber();
}

// A scalar parameter passed by value lives in the slot it was stored in at
// entry, and its address is that slot's: the one store into the slot is of
// the argument, and nothing else writes it.
std::optional<unsigned> slotOfParam(Value addr, cir::FuncOp f) {
    auto slot = traceToLocalRoot(addr).getDefiningOp<cir::AllocaOp>();
    if (!slot) return std::nullopt;
    std::optional<unsigned> p;
    for (Operation *u : slot->getUsers()) {
        auto st = dyn_cast<cir::StoreOp>(u);
        if (!st) continue;
        if (st.getAddr() != slot.getResult()) return std::nullopt;
        auto arg = dyn_cast<BlockArgument>(st.getValue());
        if (p || !arg || arg.getOwner() != &f.getBody().front()) return std::nullopt;
        p = arg.getArgNumber();
    }
    return p;
}

// Reads one function of a candidate class into statements, or says why it
// cannot.
class Reader {
public:
    Reader(Candidate &c, cir::FuncOp f, ModuleOp module) : c(c), f(f), module(module) {}

    std::vector<Stmt> stmts;
    std::string why;  // the first reason the function is not one of the five shapes
    llvm::SmallVector<cir::FuncOp, 1> preds;

    bool read() {
        Region &body = f.getBody();
        if (body.empty()) return fail("has no body here");
        walk(body);
        return why.empty();
    }

private:
    Candidate &c;
    cir::FuncOp f;
    ModuleOp module;
    bool held = false;
    unsigned cs = 0, nextCs = 0;
    Value lockSlot;
    Operation *lockCall = nullptr;
    llvm::DenseMap<Value, std::optional<bool>> retval;  // slot -> the constant last stored
    llvm::DenseMap<Value, Value> storedIn;              // slot -> the value last stored
    llvm::DenseSet<Value> getterResults;                // a ring's size or overrun count, read
    llvm::DenseSet<Value> pushedFlags;                  // slots a guarded push sets
    Value timedResult;                                  // what the timed wait answered

    // What a call on the container does: its name for a standard container,
    // what its body computes for a ring (LiftedBounded.h), "" for a ring's
    // function that computes none of a ring's operations.
    std::string opName(cir::FuncOp callee) {
        if (c.kind != Kind::Ring || !c.ring) return nameOf(callee.getSymName()).base;
        std::string reason;
        switch (c.ring->classify(callee, reason)) {
        case RingOp::Push: return "push_back";
        case RingOp::Front: return "front";
        case RingOp::PopFront: return "pop_front";
        case RingOp::Empty: return "empty";
        case RingOp::Full: return "full";
        case RingOp::Size: return "size";
        case RingOp::OverrunCounter: return "overrun_counter";
        case RingOp::ResetOverrunCounter: return "reset_overrun_counter";
        case RingOp::None: break;
        }
        c.refuse("(e) the ring's " + shortName(callee) + " " + reason);
        return "";
    }

    bool fail(const Twine &w) {
        if (why.empty()) why = w.str();
        return false;
    }
    std::string fname() const { return shortName(f); }

    std::optional<unsigned> member(Value addr) { return memberOfThis(addr, f, c.rec); }
    bool isCv(unsigned i) const { return llvm::is_contained(c.cvs, i); }

    void push(Stmt s) {
        s.held = held;
        s.cs = cs;
        stmts.push_back(s);
    }

    void walk(Region &r) {
        if (r.empty()) return;
        // Only the entry block runs: what follows a return in CIR is a
        // block with no predecessors.
        Block &b = r.front();
        // A failure does not stop the walk: the rest of the function may
        // hold reasons of its own, and the report names them all.
        for (auto it = b.begin(); it != b.end(); ++it) step(&*it, it, b);
    }

    void step(Operation *op, Block::iterator &it, Block &b) {
        if (isa<cir::AllocaOp, cir::ConstantOp, cir::CastOp, cir::NotOp, cir::YieldOp, cir::TrapOp,
                cir::UnreachableOp, cir::CmpOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(op))
            return;
        if (auto gm = dyn_cast<cir::GetMemberOp>(op)) return getMember(gm);
        if (auto ld = dyn_cast<cir::LoadOp>(op)) return;  // values are judged where they are used
        if (auto st = dyn_cast<cir::StoreOp>(op)) return store(st);
        if (auto sc = dyn_cast<cir::ScopeOp>(op)) return walk(sc.getRegion());
        if (auto cl = dyn_cast<cir::CleanupScopeOp>(op)) return cleanup(cl);
        if (auto call = dyn_cast<cir::CallOp>(op)) return this->call(call, it, b);
        if (auto w = dyn_cast<cir::WhileOp>(op)) return whileLoop(w);
        if (isa<cir::ForOp, cir::DoWhileOp>(op)) return otherLoop(op);
        if (auto i = dyn_cast<cir::IfOp>(op)) return guard(i);
        if (auto t = dyn_cast<cir::TernaryOp>(op)) {
            if (!pure(t.getTrueRegion()) || !pure(t.getFalseRegion()))
                fail("computes a condition with side effects");
            return;
        }
        if (auto ret = dyn_cast<cir::ReturnOp>(op)) return returnOp(ret);
        if (auto cp = dyn_cast<cir::CopyOp>(op)) return copy(cp);
        fail("does something other than a queue operation (" + op->getName().getStringRef() + ")");
    }

    void getMember(cir::GetMemberOp gm) {
        if (!held) return;
        auto m = member(gm.getResult());
        if (!m || *m == c.q || (c.mutex && *m == *c.mutex) || isCv(*m) || (c.stop && *m == *c.stop)) return;
        if (isStopType(c.rec.getElementType(*m)) && !gm.getResult().use_empty()) return;  // judged at its use
        if (gm.getResult().use_empty()) return;
        c.refuse("(c) the mutex " + c.memberName(*c.mutex) + " also guards " + c.memberName(*m) + " in " +
                 fname());
        fail("touches " + c.memberName(*m) + " under the mutex");
    }

    void store(cir::StoreOp st) {
        // The retval slot, and the slots parameters arrive in.
        if (auto m = member(st.getAddr())) {
            if (isStopType(c.rec.getElementType(*m))) {
                auto k = st.getValue().getDefiningOp<cir::ConstantOp>();
                auto b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
                if (!b || !b.getValue()) {
                    if (held)
                        c.refuse("(c) the mutex " + c.memberName(*c.mutex) + " also guards " + c.memberName(*m) +
                                 " in " + fname());
                    fail("writes " + c.memberName(*m) + " with something other than true");
                    return;
                }
                if (!held) {
                    c.refuse("(c) the flag " + c.memberName(*m) + " is set without the mutex in " + fname());
                    fail("sets a flag without the mutex");
                    return;
                }
                Stmt s(Stmt::SetStop);
                s.op = st;
                s.param = *m;  // which flag; checked against the one the waits read
                push(s);
                return;
            }
            if (held)
                c.refuse("(c) the mutex " + c.memberName(*c.mutex) + " also guards " + c.memberName(*m) + " in " +
                         fname());
            fail("writes the member " + c.memberName(*m));
            return;
        }
        if (auto slot = localSlot(st.getAddr())) {
            // A local: the retval slot's constants are remembered, every
            // other local is the function's own business.
            auto k = st.getValue().getDefiningOp<cir::ConstantOp>();
            auto b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
            retval[slot.getResult()] = b ? std::optional<bool>(b.getValue()) : std::nullopt;
            storedIn[slot.getResult()] = st.getValue();
            return;
        }
        // A store through the destination parameter: a scalar element
        // copied out of the front.
        if (auto p = paramOf(st.getAddr(), f)) {
            auto ld = st.getValue().getDefiningOp<cir::LoadOp>();
            if (ld && isFront(ld.getAddr())) {
                Stmt s(Stmt::Out);
                s.op = st;
                s.param = *p;
                s.front = traceToLocalRoot(ld.getAddr());
                s.move = false;
                push(s);
                return;
            }
        }
        fail("writes memory other than its own locals");
    }

    void copy(cir::CopyOp cp) {
        auto p = paramOf(cp.getDst(), f);
        if (p && isFront(cp.getSrc())) {
            Stmt s(Stmt::Out);
            s.op = cp;
            s.param = *p;
            s.front = traceToLocalRoot(cp.getSrc());
            s.move = false;
            push(s);
            return;
        }
        if (localSlot(cp.getDst())) return;
        fail("copies memory other than into its own locals");
    }

    bool isFront(Value v) {
        v = traceToLocalRoot(v);
        auto call = v.getDefiningOp<cir::CallOp>();
        if (!call || call.getNumOperands() == 0) return false;
        auto m = member(call.getOperand(0));
        if (!m || *m != c.q) return false;
        cir::FuncOp callee = lookupCallee(call);
        return callee && opName(callee) == "front";
    }

    void cleanup(cir::CleanupScopeOp cl) {
        // The body runs with whatever the cleanup releases still held; a
        // cleanup that destroys the lock ends the critical section.
        walk(cl.getBodyRegion());
        if (!why.empty()) return;
        Region &cr = cl.getCleanupRegion();
        bool releases = false;
        if (!cr.empty())
            for (Operation &op : cr.front()) {
                if (isa<cir::YieldOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(op)) continue;
                auto call = dyn_cast<cir::CallOp>(op);
                cir::FuncOp callee = call ? lookupCallee(call) : cir::FuncOp();
                if (call && callee && call.getNumOperands() == 1 && lockSlot &&
                    traceToLocalRoot(call.getOperand(0)) == lockSlot && nameOf(callee.getSymName()).dtor) {
                    releases = true;
                    continue;
                }
                if (call && callee && call.getNumOperands() == 1 && nameOf(callee.getSymName()).dtor &&
                    member(call.getOperand(0)) && *member(call.getOperand(0)) == c.q &&
                    cl.getCleanupKind() == cir::CleanupKind::EH)
                    continue;  // the constructor's unwinding path; judged with the constructor
                fail("runs a cleanup other than releasing the lock");
                return;
            }
        if (releases) {
            held = false;
            lockSlot = Value();
        }
    }

    // A lock taken on the class's mutex, by lock_guard, unique_lock or
    // scoped_lock: the critical section is what the cleanup that
    // destroys the lock covers.
    bool lockCtor(cir::CallOp call, const Name &n) {
        StringRef ctx = n.context;
        if (!n.ctor || !(ctx.starts_with("std::lock_guard<") || ctx.starts_with("std::unique_lock<") ||
                         ctx.starts_with("std::scoped_lock<")))
            return false;
        if (call.getNumOperands() != 2) {
            fail("takes a lock in a form other than on one mutex");
            return true;
        }
        auto m = member(call.getOperand(1));
        if (!m || !c.mutex || *m != *c.mutex) {
            fail("takes a lock on something other than " +
                 (c.mutex ? c.memberName(*c.mutex) : StringRef("the class's mutex")));
            return true;
        }
        if (held) {
            fail("takes the mutex twice");
            return true;
        }
        held = true;
        cs = ++nextCs;
        lockSlot = traceToLocalRoot(call.getOperand(0));
        lockCall = call;
        return true;
    }

    void call(cir::CallOp call, Block::iterator &it, Block &b) {
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) {
            fail("makes an indirect call");
            return;
        }
        const Name n = nameOf(callee.getSymName());
        if (lockCtor(call, n)) return;
        // A lock's own unlock and lock.
        if (call.getNumOperands() >= 1 && lockSlot && traceToLocalRoot(call.getOperand(0)) == lockSlot) {
            if (n.base == "unlock" && held) {
                held = false;
                return;
            }
            if (n.dtor) {
                held = false;
                lockSlot = Value();
                return;
            }
            fail("calls " + n.base + " on its lock");
            return;
        }
        if (call.getNumOperands() == 0) {
            fail("calls " + pretty(callee.getSymName()));
            return;
        }
        auto m = member(call.getOperand(0));
        if (m && *m == c.q) return containerCall(call, n);
        if (m && isCv(*m)) return cvCall(call, n, *m);
        if (m && c.mutex && *m == *c.mutex) {
            if (n.base == "lock" && !held) {
                held = true;
                cs = ++nextCs;
                return;
            }
            if (n.base == "unlock" && held && !lockSlot) {
                held = false;
                return;
            }
            fail("calls " + n.base + " on the mutex");
            return;
        }
        if (m && c.stop && *m == *c.stop) {
            fail("calls " + n.base + " on the stop flag");
            return;
        }
        // The element moved or copied out of the front into the
        // destination parameter.
        if (n.base == "operator=" && call.getNumOperands() == 2 && isFront(call.getOperand(1))) {
            auto p = paramOf(call.getOperand(0), f);
            if (!p) {
                fail("assigns the front to something other than its parameter");
                return;
            }
            Stmt s(Stmt::Out);
            s.op = call;
            s.param = *p;
            s.front = traceToLocalRoot(call.getOperand(1));
            s.move = StringRef(n.params).ends_with("&&)");
            push(s);
            return;
        }
        // The predicate object's own constructor, and std::move, touch
        // only locals.
        if (n.base == "move" && n.context == "std") return;
        // A local object built or destroyed from constants and locals,
        // of a type whose constructors and destructor are known to do
        // nothing else: a lock, a duration for a timed wait. The rewrite
        // replaces the whole body, so a constructor or destructor of any
        // other type, which may have effects of its own, refuses.
        if ((n.ctor || n.dtor) && localSlot(call.getOperand(0)) &&
            llvm::all_of(call.getOperands().drop_front(), [&](Value v) {
                return v.getDefiningOp<cir::ConstantOp>() || localSlot(v);
            })) {
            if (effectFreeLocal(n.context)) return;
            fail("builds or destroys a local " + n.context + ", whose constructor or destructor may have effects "
                 "of its own");
            return;
        }
        if (m) {
            if (held) {
                c.refuse("(c) the mutex " + c.memberName(*c.mutex) + " also guards " + c.memberName(*m) +
                         " in " + fname());
            }
            fail("calls " + pretty(callee.getSymName()) + " on " + c.memberName(*m));
            return;
        }
        fail("calls " + pretty(callee.getSymName()));
    }

    void containerCall(cir::CallOp call, const Name &n) {
        const std::string bs = opName(lookupCallee(call));
        if (bs.empty()) return (void)fail("calls " + n.base + ", which the pass cannot read, on the ring");
        StringRef b = bs;
        c.features[b.str()]++;
        if (!held) {
            if (why.empty())
                c.refuse("(b) " + fname() + " calls " + b + " on " + c.memberName(c.q) + " without the mutex held");
            fail("touches the container without the mutex");
            return;
        }
        if (b == "empty") return;  // judged where the answer is used
        if (c.kind == Kind::Ring) {
            if (b == "full") return;  // judged where the answer is used
            if (b == "size" || b == "overrun_counter") {
                Stmt s(Stmt::Getter);
                s.op = call;
                s.getter = b == "size" ? RingOp::Size : RingOp::OverrunCounter;
                getterResults.insert(call.getResult());
                push(s);
                return;
            }
            if (b == "reset_overrun_counter") {
                Stmt s(Stmt::Reset);
                s.op = call;
                push(s);
                return;
            }
        }
        if (b == "front") {
            if (call.getNumOperands() != 1) return (void)fail("calls front with arguments");
            Stmt s(Stmt::Front);
            s.op = call;
            s.front = call.getResult();
            if (auto pt = dyn_cast<cir::PointerType>(call.getResult().getType())) c.frontElem = pt.getPointee();
            push(s);
            return;
        }
        if (b == "pop" || b == "pop_front") {
            if (call.getNumOperands() != 1) return (void)fail("calls " + b + " with arguments");
            Stmt s(Stmt::PopFront);
            s.op = call;
            push(s);
            return;
        }
        if (b == "push" || b == "push_back" || b == "emplace" || b == "emplace_back") {
            if (call.getNumOperands() != 2) {
                c.refuse("(e) " + fname() + " constructs the element in place from " +
                         Twine(call.getNumOperands() - 1) + " arguments");
                return (void)fail("emplaces from several arguments");
            }
            Value v = call.getOperand(1);
            auto pt = dyn_cast<cir::PointerType>(v.getType());
            if (!pt) return (void)fail("pushes a value that is not passed by reference");
            if (c.elem && c.elem != pt.getPointee()) {
                c.refuse("(e) " + fname() + " constructs the element from another type");
                return (void)fail("pushes another type");
            }
            c.elem = pt.getPointee();
            if (b == "emplace" || b == "emplace_back") c.emplaces = true;
            Stmt s(Stmt::Push);
            s.op = call;
            s.move = StringRef(n.params).ends_with("&&)");
            if (auto p = paramOf(v, f)) {
                s.param = *p;
            } else if (auto p = slotOfParam(v, f)) {
                s.param = *p;
                s.byValueSlot = true;
            } else {
                return (void)fail("pushes something other than its parameter");
            }
            push(s);
            return;
        }
        if (b == "back" || b == "pop_back")
            c.refuse("(e) " + fname() + " takes from the back (" + b + "): LIFO, not FIFO");
        else if (b == "push_front" || b == "emplace_front")
            c.refuse("(e) " + fname() + " inserts at the front (" + b + ")");
        else if (b == "insert" || b == "erase" || b == "assign" || b == "remove" || b == "remove_if")
            c.refuse("(e) " + fname() + " inserts or removes in the middle (" + b + ")");
        else if (b == "swap")
            c.refuse("(e) " + fname() + " drains the container by swap");
        else if (b == "size")
            c.refuse("(d) " + fname() + " reads size(): the substitutes keep no linearisable count");
        else if (b == "full")
            c.refuse("(e) " + fname() + " asks full(): the container is bounded");
        else
            c.refuse("(e) " + fname() + " calls " + b + " on " + c.memberName(c.q));
        fail("calls " + b + " on the container");
    }

    void cvCall(cir::CallOp call, const Name &n, unsigned cv) {
        StringRef b = n.base;
        if (b == "notify_one" || b == "notify_all") {
            Stmt s(Stmt::Notify);
            s.op = call;
            s.cv = cv;
            s.all = b == "notify_all";
            push(s);
            return;
        }
        if (b == "wait_for" && call.getNumOperands() == 4 && c.kind == Kind::Ring) {
            c.features["timed wait"]++;
            return timedWait(call, cv);
        }
        if (b == "wait_for" || b == "wait_until") {
            c.features["timed wait"]++;
            c.refuse("(d) " + fname() + " waits with a time-out (" + b + "): the time-out path has no faithful "
                     "equivalent yet");
            return (void)fail("waits with a time-out");
        }
        if (b == "wait" && call.getNumOperands() == 3) {
            // wait(lock, predicate): the library's `while (!p()) wait(lock)`
            // with p the predicate object's call operator.
            if (!held || traceToLocalRoot(call.getOperand(1)) != lockSlot)
                return (void)fail("waits with a lock other than the one it holds");
            cir::FuncOp waitFn = lookupCallee(call);
            cir::FuncOp pred;
            if (waitFn && !waitFn.getBody().empty())
                waitFn.walk([&](cir::CallOp inner) {
                    cir::FuncOp g = lookupCallee(inner);
                    if (g && !pred && nameOf(g.getSymName()).base == "operator()") pred = g;
                });
            if (!pred) return (void)fail("waits on a predicate the pass cannot read");
            std::optional<Table> t = predicate(pred);
            if (!t) return;
            Stmt s(Stmt::Wait);
            s.op = call;
            s.cv = cv;
            s.table = *t;
            preds.push_back(pred);
            push(s);
            return;
        }
        if (b == "wait") {
            c.refuse("(d) " + fname() + " waits outside a predicate loop");
            return (void)fail("waits outside a predicate loop");
        }
        fail("calls " + b + " on a condition variable");
    }

    // A loop that is not the predicate loop: what it does is still read,
    // for the report, and the function is none of the five operations.
    void otherLoop(Operation *op) {
        fail(held ? "loops in the critical section" : "loops");
        for (Region &r : op->getRegions()) walk(r);
    }

    // `while (cond) cv.wait(lock);`
    void whileLoop(cir::WhileOp w) {
        if (!w.getCleanup().empty()) return (void)fail("has a loop with a cleanup");
        if (!held) return otherLoop(w);
        Region &cond = w.getCond();
        auto term = dyn_cast<cir::ConditionOp>(cond.front().getTerminator());
        if (!term || !pure(cond)) return (void)fail("has a loop whose condition has side effects");
        std::optional<Table> stay = table(term.getCondition());
        if (!stay) return;
        // The body: one wait on the lock, in scopes.
        cir::CallOp wait;
        bool other = false;
        w.getBody().walk([&](Operation *op) {
            if (isa<cir::ScopeOp, cir::YieldOp, cir::LoadOp>(op)) return;
            auto call = dyn_cast<cir::CallOp>(op);
            if (auto gm = dyn_cast<cir::GetMemberOp>(op); gm && member(gm.getResult())) return;
            cir::FuncOp callee = call ? lookupCallee(call) : cir::FuncOp();
            if (call && callee && !wait && call.getNumOperands() == 2 &&
                nameOf(callee.getSymName()).base == "wait") {
                wait = call;
                return;
            }
            if (call && callee) {
                const std::string b = nameOf(callee.getSymName()).base;
                if (b == "wait_for" || b == "wait_until") {
                    c.features["timed wait"]++;
                    c.refuse("(d) " + fname() + " waits with a time-out (" + b +
                             "): the time-out path has no faithful equivalent yet");
                }
            }
            other = true;
        });
        if (other || !wait) {
            c.refuse("(d) " + fname() + " loops on something other than one wait");
            return (void)fail("has a loop that is not a predicate loop");
        }
        auto cv = member(wait.getOperand(0));
        if (!cv || !isCv(*cv) || traceToLocalRoot(wait.getOperand(1)) != lockSlot)
            return (void)fail("waits in a loop on something other than its condition variable and lock");
        Stmt s(Stmt::Wait);
        s.op = w;
        s.cv = *cv;
        s.table = ~*stay & kAll;
        push(s);
    }

    // The scope that is the critical section the lock taken last guards:
    // the one that holds the lock's own slot.
    Operation *sectionOfLock() {
        if (!lockCall || !lockSlot) return nullptr;
        Operation *slot = lockSlot.getDefiningOp();
        for (Operation *p = lockCall->getParentOp(); p && p != f.getOperation(); p = p->getParentOp())
            if (isa<cir::ScopeOp>(p) && slot && p->isAncestor(slot)) return p;
        return nullptr;
    }

    // Whether a region touches the queue's members: the container, the
    // mutex, a condition variable or the flag.
    bool touchesQueue(Region &r) {
        bool touches = false;
        r.walk([&](cir::GetMemberOp gm) {
            auto m = member(gm.getResult());
            if (m && (*m == c.q || (c.mutex && *m == *c.mutex) || isCv(*m) || (c.stop && *m == *c.stop) ||
                      isStopType(c.rec.getElementType(*m))))
                touches = true;
        });
        return touches;
    }

    // After the critical section of a push that may decline: `if (pushed)
    // cv.notify_one(); else <the program's own code>`.
    bool flagIf(cir::IfOp i) {
        if (held) return false;
        auto ld = traceToLocalRoot(i.getCondition()).getDefiningOp<cir::LoadOp>();
        Value slot = ld ? traceToLocalRoot(ld.getAddr()) : Value();
        if (!slot || !pushedFlags.contains(slot)) return false;
        Stmt s(Stmt::FlagIf);
        s.op = i;
        s.flag = slot;
        for (Operation &op : i.getThenRegion().front()) {
            if (isa<cir::YieldOp, cir::GetMemberOp>(op)) continue;
            auto call = dyn_cast<cir::CallOp>(op);
            cir::FuncOp callee = call ? lookupCallee(call) : cir::FuncOp();
            auto m = call && call.getNumOperands() == 1 ? member(call.getOperand(0)) : std::nullopt;
            const std::string b = callee ? nameOf(callee.getSymName()).base : "";
            if (m && isCv(*m) && (b == "notify_one" || b == "notify_all")) {
                Stmt n(Stmt::Notify);
                n.op = call;
                n.cv = *m;
                n.all = b == "notify_all";
                push(n);
                continue;
            }
            fail("does more than notify after a push that may decline");
            return true;
        }
        if (!i.getElseRegion().empty() && touchesQueue(i.getElseRegion())) {
            fail("touches the queue where a push has declined");
            return true;
        }
        push(s);
        return true;
    }

    // `if (!cv.wait_for(lock, d, pred)) return false;`
    bool timedOut(cir::IfOp i) {
        if (!timedResult) return false;
        auto n = traceToLocalRoot(i.getCondition()).getDefiningOp<cir::NotOp>();
        if (!n || traceToLocalRoot(n.getInput()) != timedResult) return false;
        timedResult = Value();
        std::optional<bool> value;
        auto saved = retval;
        for (Operation &op : i.getThenRegion().front()) {
            if (isa<cir::ConstantOp, cir::YieldOp, cir::LoadOp>(op)) continue;
            if (auto st = dyn_cast<cir::StoreOp>(op)) {
                auto slot = traceToLocalRoot(st.getAddr()).getDefiningOp<cir::AllocaOp>();
                auto k = st.getValue().getDefiningOp<cir::ConstantOp>();
                auto b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
                if (!slot || !b) break;
                retval[slot.getResult()] = b.getValue();
                continue;
            }
            if (auto ret = dyn_cast<cir::ReturnOp>(op); ret && ret.getNumOperands() == 1) {
                value = constantReturned(ret.getOperand(0));
                break;
            }
            break;
        }
        retval = saved;
        if (!value || *value || !i.getElseRegion().empty()) {
            fail("does something other than return false when its timed wait ends without an element");
            return true;
        }
        Stmt s(Stmt::TimedOut);
        s.op = i;
        s.value = false;
        push(s);
        return true;
    }

    // Inside the critical section: `if (!q.full()) { q.push_back(v);
    // pushed = true; }`.
    bool guardedPush(cir::IfOp i) {
        if (!held || c.kind != Kind::Ring || i.getThenRegion().empty()) return false;
        cir::CallOp pushCall;
        for (Operation &op : i.getThenRegion().front())
            if (auto call = dyn_cast<cir::CallOp>(op); call && call.getNumOperands() == 2) {
                auto m = member(call.getOperand(0));
                cir::FuncOp callee = lookupCallee(call);
                if (m && *m == c.q && callee && opName(callee) == "push_back") pushCall = call;
            }
        if (!pushCall) return false;
        c.features["push_back"]++;
        Stmt s(Stmt::GuardedPush);
        s.op = i;
        if (!i.getElseRegion().empty()) return fail("has an else beside a push that may decline"), true;
        std::optional<Table> t = table(i.getCondition());
        if (!t) return true;
        s.table = *t;
        for (Operation &op : i.getThenRegion().front()) {
            if (isa<cir::YieldOp, cir::ConstantOp, cir::LoadOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(op) ||
                &op == pushCall.getOperation())
                continue;
            // std::move of the parameter.
            if (auto call = dyn_cast<cir::CallOp>(op); call && lookupCallee(call) &&
                                                     nameOf(lookupCallee(call).getSymName()).base == "move" &&
                                                     nameOf(lookupCallee(call).getSymName()).context == "std")
                continue;
            if (auto gm = dyn_cast<cir::GetMemberOp>(op); gm && member(gm.getResult()) == c.q) continue;
            if (auto st = dyn_cast<cir::StoreOp>(op)) {
                auto slot = localSlot(st.getAddr());
                auto k = st.getValue().getDefiningOp<cir::ConstantOp>();
                auto b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
                if (slot && b && b.getValue() && !s.flag && slot.getResult() != lockSlot) {
                    s.flag = slot.getResult();
                    continue;
                }
            }
            return fail("does more than push and note it where there is room (" + op.getName().getStringRef() + ")"),
                   true;
        }
        Value v = pushCall.getOperand(1);
        auto pt = dyn_cast<cir::PointerType>(v.getType());
        if (!pt) return fail("pushes a value that is not passed by reference"), true;
        if (c.elem && c.elem != pt.getPointee()) {
            c.refuse("(e) " + fname() + " constructs the element from another type");
            return fail("pushes another type"), true;
        }
        c.elem = pt.getPointee();
        cir::FuncOp callee = lookupCallee(pushCall);
        s.move = StringRef(nameOf(callee.getSymName()).params).ends_with("&&)");
        if (auto p = paramOf(v, f)) {
            s.param = *p;
        } else if (auto p = slotOfParam(v, f)) {
            s.param = *p;
            s.byValueSlot = true;
        } else {
            return fail("pushes something other than its parameter"), true;
        }
        s.section = sectionOfLock();
        if (!s.section) return fail("takes its lock for the whole function around a push that may decline"), true;
        if (s.flag) pushedFlags.insert(s.flag);
        push(s);
        return true;
    }

    // `cv.wait_for(lock, d, pred)` on a ring, d the function's parameter.
    void timedWait(cir::CallOp call, unsigned cv) {
        if (!held || traceToLocalRoot(call.getOperand(1)) != lockSlot)
            return (void)fail("waits with a lock other than the one it holds");
        // The predicate's call operator, inside wait_for or what it calls.
        cir::FuncOp pred;
        llvm::SmallVector<std::pair<cir::FuncOp, unsigned>, 4> work{{lookupCallee(call), 0}};
        llvm::DenseSet<Operation *> seen;
        while (!work.empty() && !pred) {
            auto [g, dd] = work.pop_back_val();
            const unsigned d = dd;
            if (!g || g.getBody().empty() || !seen.insert(g).second || d > 3) continue;
            g.walk([&](cir::CallOp inner) {
                cir::FuncOp h = lookupCallee(inner);
                if (!h || pred) return;
                if (nameOf(h.getSymName()).base == "operator()" && StringRef(pretty(h.getSymName())).contains("lambda"))
                    pred = h;
                else
                    work.push_back({h, d + 1});
            });
        }
        if (!pred) return (void)fail("waits on a predicate the pass cannot read");
        std::optional<Table> t = predicate(pred);
        if (!t) return;
        Value d = traceToLocalRoot(call.getOperand(2));
        auto slot = d.getDefiningOp<cir::AllocaOp>();
        std::optional<unsigned> p = slot ? slotOfParam(d, f) : std::nullopt;
        // A duration arrives coerced to an integer: the argument is stored
        // into a slot of its own, read back as the duration, and that is
        // stored into the parameter's slot.
        if (slot && !p) {
            cir::StoreOp only;
            for (Operation *u : slot->getUsers())
                if (auto st = dyn_cast<cir::StoreOp>(u); st && st.getAddr() == slot.getResult())
                    only = only ? cir::StoreOp() : st;
            auto ld = only ? only.getValue().getDefiningOp<cir::LoadOp>() : cir::LoadOp();
            Value coerce = ld ? traceToLocalRoot(ld.getAddr()) : Value();
            if (coerce && coerce.getDefiningOp<cir::AllocaOp>()) p = slotOfParam(coerce, f);
        }
        if (!p) return (void)fail("waits for a time other than its parameter");
        StringRef ty = recordName(slot.getAllocaType());
        // CIR spells the ratio's arguments with their suffix: 1000L.
        std::string norm;
        for (size_t i = 0; i < ty.size(); ++i)
            if (!(ty[i] == 'L' && i > 0 && llvm::isDigit(ty[i - 1]))) norm += ty[i];
        StringRef n(norm);
        std::string unit;
        if (n.ends_with("std::ratio<1, 1000000000>>")) unit = "ns";
        else if (n.ends_with("std::ratio<1, 1000000>>")) unit = "us";
        else if (n.ends_with("std::ratio<1, 1000>>")) unit = "ms";
        else if (n == "std::chrono::duration<long>" || n.ends_with("std::ratio<1, 1>>")) unit = "s";
        if (unit.empty()) {
            c.refuse("(d) " + fname() + " waits for a " + ty + ", a unit the runtime does not take");
            return (void)fail("waits for a time in another unit");
        }
        Stmt s(Stmt::TimedWait);
        s.op = call;
        s.cv = cv;
        s.table = *t;
        s.durationParam = *p;
        s.unit = unit;
        timedResult = call.getResult();
        preds.push_back(pred);
        push(s);
    }

    // `if (cond) return <constant>;`
    void guard(cir::IfOp i) {
        if (flagIf(i) || timedOut(i) || guardedPush(i)) return;
        // `if (empty) cv.wait(lock);`: a wait that a spurious wake-up
        // leaves with the queue still empty.
        bool waits = false;
        i->walk([&](cir::CallOp call) {
            cir::FuncOp callee = lookupCallee(call);
            auto m = call.getNumOperands() ? member(call.getOperand(0)) : std::nullopt;
            if (!callee || !m || !isCv(*m)) return;
            const std::string b = nameOf(callee.getSymName()).base;
            if (b == "wait_for" || b == "wait_until") {
                c.features["timed wait"]++;
                c.refuse("(d) " + fname() + " waits with a time-out (" + b +
                         "): the time-out path has no faithful equivalent yet");
                waits = true;
            } else if (b == "wait" && call.getNumOperands() == 2) {
                c.refuse("(d) " + fname() + " waits outside a predicate loop");
                waits = true;
            } else if (b == "wait") {
                c.refuse("(d) " + fname() + " waits on one branch of an if");
                waits = true;
            }
        });
        if (waits) return (void)fail("waits in an if");
        if (!i.getElseRegion().empty()) return (void)fail("has an if with an else");
        std::optional<Table> t = table(i.getCondition());
        if (!t) return;
        Region &then = i.getThenRegion();
        std::optional<bool> value;
        bool returns = false;
        auto saved = retval;
        for (Operation &op : then.front()) {
            if (isa<cir::ConstantOp, cir::YieldOp>(op)) continue;
            if (auto st = dyn_cast<cir::StoreOp>(op)) {
                auto slot = traceToLocalRoot(st.getAddr()).getDefiningOp<cir::AllocaOp>();
                auto k = st.getValue().getDefiningOp<cir::ConstantOp>();
                auto b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
                if (!slot || !b) return (void)fail("has an if that does more than return");
                retval[slot.getResult()] = b.getValue();
                continue;
            }
            if (isa<cir::LoadOp>(op)) continue;
            if (auto ret = dyn_cast<cir::ReturnOp>(op)) {
                returns = true;
                if (ret.getNumOperands() == 1) {
                    value = constantReturned(ret.getOperand(0));
                    if (!value) return (void)fail("returns something other than a constant");
                }
                continue;
            }
            return (void)fail("has an if that does more than return");
        }
        retval = saved;
        if (!returns) return (void)fail("has an if that does not return");
        Stmt s(Stmt::Guard);
        s.op = i;
        s.table = *t;
        s.value = value;
        push(s);
    }

    std::optional<bool> constantReturned(Value v) {
        if (auto k = v.getDefiningOp<cir::ConstantOp>())
            if (auto b = dyn_cast<cir::BoolAttr>(k.getValue())) return b.getValue();
        if (auto ld = v.getDefiningOp<cir::LoadOp>()) {
            auto it = retval.find(traceToLocalRoot(ld.getAddr()));
            if (it != retval.end()) return it->second;
        }
        return std::nullopt;
    }

    void returnOp(cir::ReturnOp ret) {
        Stmt s(Stmt::Return);
        s.op = ret;
        if (ret.getNumOperands() == 1) {
            s.returnsSomething = true;
            // A ring's size or overrun count, returned as read.
            Value v = ret.getOperand(0);
            if (auto ld = v.getDefiningOp<cir::LoadOp>()) {
                auto it = storedIn.find(traceToLocalRoot(ld.getAddr()));
                if (it != storedIn.end()) v = it->second;
            }
            if (getterResults.contains(v)) {
                s.fromGetter = true;
                push(s);
                return;
            }
            s.value = constantReturned(ret.getOperand(0));
            if (!s.value) return (void)fail("returns something other than a constant");
        }
        push(s);
    }

    // Whether a region computes a value and nothing else: member addresses,
    // loads, constants, logic, and the container's empty().
    bool pure(Region &r) {
        bool ok = true;
        r.walk([&](Operation *op) {
            if (isa<cir::GetMemberOp, cir::LoadOp, cir::ConstantOp, cir::NotOp, cir::CastOp, cir::YieldOp,
                    cir::ConditionOp, cir::TernaryOp, cir::CmpOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(op))
                return;
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                cir::FuncOp callee = lookupCallee(call);
                auto m = call.getNumOperands() ? member(call.getOperand(0)) : std::nullopt;
                if (callee && m && *m == c.q && (opName(callee) == "empty" || opName(callee) == "full")) return;
                if (callee && m && c.stop && *m == *c.stop) return;  // std::atomic<bool>'s load
                if (callee && m && isStopType(c.rec.getElementType(*m))) return;
            }
            ok = false;
        });
        return ok;
    }

    // The truth table of a boolean over (empty, stopped), or none with the
    // reason when it reads anything else.
    std::optional<Table> table(Value v) {
        Table t = 0;
        for (bool full : {false, true})
            for (bool e : {false, true})
                for (bool s : {false, true}) {
                    std::optional<bool> r = eval(v, e, s, full, 0);
                    if (!r) return std::nullopt;
                    if (*r) t |= bitOf(e, s, full);
                }
        return t;
    }

    std::optional<bool> eval(Value v, bool e, bool s, bool full, unsigned depth) {
        if (depth > 32) return fail("has a condition too deep to read"), std::nullopt;
        Operation *d = v.getDefiningOp();
        if (!d) return fail("has a condition the pass cannot read"), std::nullopt;
        if (auto k = dyn_cast<cir::ConstantOp>(d)) {
            if (auto b = dyn_cast<cir::BoolAttr>(k.getValue())) return b.getValue();
            return fail("has a condition the pass cannot read"), std::nullopt;
        }
        if (auto n = dyn_cast<cir::NotOp>(d)) {
            auto r = eval(n.getInput(), e, s, full, depth + 1);
            return r ? std::optional<bool>(!*r) : std::nullopt;
        }
        if (auto cast = dyn_cast<cir::CastOp>(d)) {
            if (cast.getKind() == cir::CastKind::int_to_bool || cast.getKind() == cir::CastKind::bool_to_int)
                return eval(cast.getSrc(), e, s, full, depth + 1);
            return fail("has a condition the pass cannot read"), std::nullopt;
        }
        if (auto t = dyn_cast<cir::TernaryOp>(d)) {
            auto c0 = eval(t.getCond(), e, s, full, depth + 1);
            if (!c0) return std::nullopt;
            Region &r = *c0 ? t.getTrueRegion() : t.getFalseRegion();
            auto y = dyn_cast<cir::YieldOp>(r.front().getTerminator());
            if (!y || y.getNumOperands() != 1) return fail("has a condition the pass cannot read"), std::nullopt;
            return eval(y.getOperand(0), e, s, full, depth + 1);
        }
        if (auto call = dyn_cast<cir::CallOp>(d)) {
            cir::FuncOp callee = lookupCallee(call);
            auto m = call.getNumOperands() == 1 ? member(call.getOperand(0)) : std::nullopt;
            if (callee && m && *m == c.q && opName(callee) == "empty") return e;
            if (callee && m && *m == c.q && c.kind == Kind::Ring && opName(callee) == "full") return full;
            if (callee && m && isStopType(c.rec.getElementType(*m))) {
                if (!stopAt(*m)) return std::nullopt;
                return s;  // std::atomic<bool>'s conversion or load
            }
            if (callee && m && *m == c.q) {
                const std::string b = opName(callee);
                c.features[b]++;
                if (b == "size")
                    c.refuse("(d) " + fname() + " reads size(): the substitutes keep no linearisable count");
                else if (b == "full")
                    c.refuse("(e) " + fname() + " asks full(): the container is bounded");
                else
                    c.refuse("(d) " + fname() + " decides on " + b + "()");
            }
            return fail("has a condition that calls " + (callee ? pretty(callee.getSymName()) : "something")),
                   std::nullopt;
        }
        if (auto ld = dyn_cast<cir::LoadOp>(d)) {
            auto m = member(ld.getAddr());
            if (m && isStopType(c.rec.getElementType(*m))) {
                if (!stopAt(*m)) return std::nullopt;
                return s;
            }
            if (m) {
                c.refuse("(c) the mutex " +
                         (c.mutex ? c.memberName(*c.mutex) : StringRef("of the class")) + " also guards " +
                         c.memberName(*m) + ", read in a condition in " + fname());
                return fail("decides on " + c.memberName(*m)), std::nullopt;
            }
            return fail("has a condition that reads a local"), std::nullopt;
        }
        return fail("has a condition the pass cannot read"), std::nullopt;
    }

    bool stopAt(unsigned m) {
        if (c.stop && *c.stop != m) {
            c.refuse("(d) the waits read two flags, " + c.memberName(*c.stop) + " and " + c.memberName(m));
            return fail("reads two flags");
        }
        c.stop = m;
        return true;
    }

public:
    // A predicate's call operator: `return <condition over the container
    // and the stop flag>;`
    std::optional<Table> predicate(cir::FuncOp p) {
        if (p.getBody().empty()) return fail("waits on a predicate with no body here"), std::nullopt;
        Block &b = p.getBody().front();
        cir::ReturnOp ret;
        llvm::DenseMap<Value, Value> stored;
        bool ok = true;
        for (Operation &op : b) {
            if (isa<cir::AllocaOp, cir::GetMemberOp, cir::LoadOp, cir::ConstantOp, cir::NotOp, cir::CastOp,
                    cir::TernaryOp, cir::CmpOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(op))
                continue;
            if (auto st = dyn_cast<cir::StoreOp>(op)) {
                if (!localSlot(st.getAddr())) ok = false;
                stored[traceToLocalRoot(st.getAddr())] = st.getValue();
                continue;
            }
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                cir::FuncOp callee = lookupCallee(call);
                auto m = call.getNumOperands() == 1 ? memberOfThis(call.getOperand(0), p, c.rec) : std::nullopt;
                if (callee && m && (*m == c.q || isStopType(c.rec.getElementType(*m)))) continue;
            }
            if (auto r = dyn_cast<cir::ReturnOp>(op)) {
                ret = r;
                continue;
            }
            ok = false;
        }
        if (!ok || !ret || ret.getNumOperands() != 1)
            return fail("waits on a predicate that does more than read the queue and a flag"), std::nullopt;
        Value v = ret.getOperand(0);
        if (auto ld = v.getDefiningOp<cir::LoadOp>()) {
            auto it = stored.find(traceToLocalRoot(ld.getAddr()));
            if (it != stored.end()) v = it->second;
        }
        // Evaluate in the predicate's own frame.
        cir::FuncOp saved = f;
        f = p;
        std::optional<Table> t = table(v);
        f = saved;
        return t;
    }
};

class LiftLockedQueuesPass : public PassWrapper<LiftLockedQueuesPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LiftLockedQueuesPass)

    LiftLockedQueuesPass() = default;
    LiftLockedQueuesPass(const LiftLockedQueuesPass &other) : PassWrapper(other) {}

    StringRef getArgument() const final { return "wtc-lift-locked-queues"; }
    StringRef getDescription() const final {
        return "Recognise classes that wrap a standard container under a mutex as queues, prove the "
               "substitution legal, and replace their operations with wtc::lifted's";
    }
    void getDependentDialects(DialectRegistry &registry) const override { registry.insert<cir::CIRDialect>(); }

    Option<bool> rewrite{*this, "rewrite", llvm::cl::init(true),
                         llvm::cl::desc("Replace the operations of every class that lifts. With false the "
                                        "pass only reports.")};
    Option<std::string> instantiations{
        *this, "instantiations", llvm::cl::init(""),
        llvm::cl::desc("Write here, one per line, the explicit instantiation of wtc::lifted for the "
                       "container type of every class that lifts and whose runtime this module lacks")};
    Option<bool> openWorld{*this, "open-world", llvm::cl::init(false),
                           llvm::cl::desc("The module is linked later with files the pass does not see, "
                                          "which may use any class as written: nothing lifts")};
    Option<std::string> namedElsewhereFile{
        *this, "named-elsewhere", llvm::cl::init(""),
        llvm::cl::desc("A file listing, one per line, the symbols the other files linked with this module "
                       "define or reference: a class any of them names does not lift")};

    void runOnOperation() override;

private:
    ModuleOp module;
    llvm::DenseMap<std::pair<Type, unsigned>, std::string> memberNames;

    // What the rest of the program may see: whether this module has main,
    // and the demangled names, spaces removed, the other files name.
    bool hasMain = false;
    std::vector<std::string> elsewhere;

    void analyse(Candidate &c);
    void closedWorld(Candidate &c);
    void boundedChecks(Candidate &c);
    bool onlyReturned(cir::FuncOp f, unsigned depth, llvm::DenseSet<Operation *> &seen);
    bool onlyDirectCalls(cir::FuncOp f, llvm::SmallVectorImpl<cir::CallOp> &calls);
    void decide(Candidate &c, Reader &r, cir::FuncOp f, bool explained);
    bool constructorOk(Candidate &c, cir::FuncOp f, bool dtor);
    llvm::StringMap<cir::FuncOp> runtimeFor(Candidate &c);
    void lift(Candidate &c, llvm::StringMap<cir::FuncOp> &rt);
    void eraseUnusedRuntime();
};

void LiftLockedQueuesPass::runOnOperation() {
    module = getOperation();
    gMemberNames = &memberNames;

    auto mainFn = module.lookupSymbol<cir::FuncOp>("main");
    hasMain = mainFn && !mainFn.getBody().empty();
    elsewhere.clear();
    if (!namedElsewhereFile.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(namedElsewhereFile.getValue());
        if (!buf) {
            module.emitError() << "wtc: cannot read the symbols named elsewhere from " << namedElsewhereFile.getValue();
            return signalPassFailure();
        }
        llvm::SmallVector<StringRef, 64> lines;
        (*buf)->getBuffer().split(lines, '\n', -1, false);
        for (StringRef l : lines)
            if (!(l = l.trim()).empty()) elsewhere.push_back(squashSpaces(llvm::demangle(l.str())));
    }

    // Member names, and the records that hold a std::mutex.
    llvm::SetVector<Type> withMutex;
    module.walk([&](cir::GetMemberOp gm) {
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
        if (!rec || rec.isIncomplete()) return;
        memberNames.try_emplace({rec, static_cast<unsigned>(gm.getIndex())}, gm.getName().str());
        for (Type m : rec.getMembers())
            if (recordName(m) == "std::mutex") withMutex.insert(rec);
    });

    // A candidate: a member of such a record, of a record type that has no
    // mutex of its own (a queue class used by another class is that class's
    // member, not its container), which something puts into and something
    // takes out of, at least once inside a function that locks the
    // record's mutex.
    struct Use {
        bool put = false, take = false, locked = false;
        Location loc;
    };
    llvm::MapVector<std::pair<Type, unsigned>, Use> uses;
    auto locksMutexOf = [&](cir::FuncOp f, cir::RecordType rec) {
        bool found = false;
        f.walk([&](cir::CallOp call) {
            cir::FuncOp callee = lookupCallee(call);
            if (found || !callee || call.getNumOperands() == 0) return;
            for (Value v : call.getOperands()) {
                auto gm = traceToLocalRoot(v).getDefiningOp<cir::GetMemberOp>();
                if (!gm) continue;
                auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
                if (pt && pt.getPointee() == rec && recordName(rec.getElementType(gm.getIndex())) == "std::mutex")
                    found = true;
            }
        });
        return found;
    };
    module.walk([&](cir::CallOp call) {
        if (call.getNumOperands() == 0) return;
        auto gm = traceToLocalRoot(call.getOperand(0)).getDefiningOp<cir::GetMemberOp>();
        if (!gm) return;
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
        if (!rec || !withMutex.contains(rec)) return;
        auto member = dyn_cast<cir::RecordType>(rec.getElementType(gm.getIndex()));
        if (!member || withMutex.contains(member) ||
            llvm::any_of(member.getMembers(), [](Type t) { return recordName(t) == "std::mutex"; }))
            return;
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return;
        const std::string b = nameOf(callee.getSymName()).base;
        const bool put = b == "push" || b == "push_back" || b == "push_front" || b == "emplace" ||
                         b == "emplace_back" || b == "emplace_front" || b == "insert";
        const bool take = b == "pop" || b == "pop_front" || b == "pop_back" || b == "front" || b == "back" ||
                          b == "erase";
        if (!put && !take) return;
        auto [it, fresh] = uses.insert({{rec, static_cast<unsigned>(gm.getIndex())}, Use{false, false, false, call.getLoc()}});
        it->second.put |= put;
        it->second.take |= take;
        if (!it->second.locked) it->second.locked = locksMutexOf(call->getParentOfType<cir::FuncOp>(), rec);
    });
    llvm::MapVector<std::pair<Type, unsigned>, Location> found;
    for (auto &[key, u] : uses)
        if (u.put && u.take && u.locked) found.insert({key, u.loc});

    std::string lines;
    for (auto &[key, loc] : found) {
        Candidate c(loc);
        c.rec = cast<cir::RecordType>(key.first);
        c.q = key.second;
        analyse(c);
        closedWorld(c);

        if (!c.features.empty()) {
            InFlightDiagnostic fd = emitRemark(c.loc) << "wtc: locked queue " << c.label() << " uses ";
            llvm::interleaveComma(c.features, fd, [&](const auto &kv) { fd << kv.first << " " << kv.second; });
        }
        if (!c.reasons.empty()) {
            for (const std::string &why : c.reasons)
                emitRemark(c.loc) << "wtc: locked queue " << c.label() << " not lifted: " << why;
            continue;
        }

        llvm::StringMap<cir::FuncOp> rt = runtimeFor(c);
        if (rt.empty()) {
            StringRef ty = recordName(c.rec.getElementType(c.q));
            if (ty.contains("(lambda") || ty.contains("(anonymous") || ty.contains("(unnamed")) {
                emitRemark(c.loc) << "wtc: locked queue " << c.label()
                                  << " not lifted: its container type cannot be named outside the file";
                continue;
            }
            lines += ("template struct wtc::lifted" + Twine(c.kind == Kind::Ring ? "_bounded" : "") + "<" + ty +
                      ">;\n")
                         .str();
            if (rewrite)
                emitRemark(c.loc) << "wtc: locked queue " << c.label()
                                  << " not lifted: wtc::lifted<" << ty
                                  << "> is not instantiated in this module (wtc-cc instantiates it)";
            else
                emitRemark(c.loc) << "wtc: locked queue " << c.label() << " lifts";
            continue;
        }
        if (rewrite) lift(c, rt);
        else emitRemark(c.loc) << "wtc: locked queue " << c.label() << " lifts";
    }
    if (rewrite) eraseUnusedRuntime();

    if (!instantiations.empty()) {
        std::error_code ec;
        llvm::raw_fd_ostream out(instantiations, ec, llvm::sys::fs::OF_Text);
        if (ec) {
            module.emitError() << "wtc: cannot write " << instantiations << ": " << ec.message();
            return signalPassFailure();
        }
        out << lines;
    }
    gMemberNames = nullptr;
}

void LiftLockedQueuesPass::analyse(Candidate &c) {
    // The members by kind.
    llvm::ArrayRef<Type> members = c.rec.getMembers();
    for (unsigned i = 0; i < members.size(); ++i) {
        StringRef n = recordName(members[i]);
        if (n == "std::mutex") {
            if (c.mutex) c.refuse("(c) the class has two mutexes, " + c.memberName(*c.mutex) + " and " + c.memberName(i));
            c.mutex = i;
        } else if (n == "std::condition_variable") {
            c.cvs.push_back(i);
        } else if (n.contains("condition_variable_any")) {
            c.refuse("(d) " + c.memberName(i) + " is a condition_variable_any");
        }
    }
    StringRef qType = recordName(members[c.q]);
    c.kind = containerKind(qType);
    switch (c.kind) {
    case Kind::StdQueue: {
        // The adapter's one member, the container it pushes at the back of
        // and pops at the front of; its type, not the adapter's spelling,
        // which a container of the program's own may mimic.
        auto adapter = dyn_cast<cir::RecordType>(members[c.q]);
        StringRef under;
        if (adapter && !adapter.isIncomplete() && adapter.getMembers().size() == 1)
            under = recordName(adapter.getMembers().front());
        const Kind k = containerKind(under);
        if (k != Kind::StdDeque && k != Kind::StdList)
            c.refuse("(e) the std::queue's underlying container is neither a std::deque nor a std::list" +
                     (under.empty() ? std::string() : (" (" + under + ")").str()));
        break;
    }
    case Kind::StdDeque:
    case Kind::StdList:
        break;
    case Kind::StdVector:
        c.refuse("(e) the container is a std::vector, which the class may use as a stack or a batch");
        break;
    case Kind::StdPriorityQueue:
        c.refuse("(e) the container is a std::priority_queue: priority order, not FIFO");
        break;
    default: {
        // A ring the class bounds itself: read from its member functions.
        auto ring = std::make_shared<RingModel>(module, cast<cir::RecordType>(members[c.q]));
        if (!ring->why().empty()) {
            c.refuse("(e) the container " + qType + " is neither a standard container nor a bounded ring: " +
                     ring->why());
            break;
        }
        c.kind = Kind::Ring;
        c.ring = ring;
        break;
    }
    }

    // Every function that touches the container, the mutex, a condition
    // variable or a flag of the class, and what the touch is.
    llvm::SetVector<cir::FuncOp> touching;
    module.walk([&](cir::GetMemberOp gm) {
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        if (!pt || pt.getPointee() != c.rec) return;
        unsigned i = gm.getIndex();
        if (i == c.q || (c.mutex && i == *c.mutex) || llvm::is_contained(c.cvs, i) ||
            isStopType(members[i]))
            touching.insert(gm->getParentOfType<cir::FuncOp>());
    });
    // The whole object copied byte for byte would copy the container too.
    module.walk([&](cir::CopyOp cp) {
        if (cp.getDst().getType().getPointee() == c.rec)
            c.refuse("(a) " + shortName(cp->getParentOfType<cir::FuncOp>()) + " copies the object whole");
    });

    llvm::SmallVector<cir::FuncOp, 4> lambdas;
    for (cir::FuncOp f : touching) {
        if (!f) continue;
        if (f.isCxxConstructor() || f.isCxxDestructor()) {
            auto args = f.getArgumentTypes();
            auto pt = args.empty() ? cir::PointerType() : dyn_cast<cir::PointerType>(args[0]);
            if (pt && pt.getPointee() == c.rec) {
                if (constructorOk(c, f, f.isCxxDestructor()))
                    (f.isCxxDestructor() ? c.dtors : c.ctors).push_back(f);
                continue;
            }
        }
        auto args = f.getArgumentTypes();
        const unsigned self = thisIndex(f);
        auto pt = args.size() <= self ? cir::PointerType() : dyn_cast<cir::PointerType>(args[self]);
        if (!pt || pt.getPointee() != c.rec || !isMemberOf(f, c.rec)) {
            lambdas.push_back(f);  // a predicate, if a wait below reads it
            continue;
        }
        Reader r(c, f, module);
        const size_t before = c.reasons.size();
        r.read();
        for (cir::FuncOp p : r.preds) c.preds.push_back(p);
        decide(c, r, f, c.reasons.size() != before);
    }
    // A function that is not a member, other than a wait's predicate. A
    // lambda inside a member function that is already refused adds
    // nothing to the report.
    for (cir::FuncOp f : lambdas)
        if (!llvm::is_contained(c.preds, f) && c.reasons.empty())
            c.refuse("(a) " + shortName(f) + " touches the members of " + recordName(c.rec) +
                     " outside its member functions");

    // The stop flag is the one the waits read; a close sets it, and a set
    // of any other flag is state the mutex guards besides the queue.
    for (const Method &m : c.methods)
        if (m.role == Role::Close && (!c.stop || *c.stop != m.flag))
            c.refuse("(c) the mutex " + c.memberName(*c.mutex) + " also guards " + c.memberName(m.flag) + " in " +
                     shortName(m.f));

    // emplace builds the element from its argument, which may be of
    // another type than the element; the substitute's push takes the
    // element itself.
    if (c.elem && (c.frontElem ? c.elem != c.frontElem : c.emplaces))
        c.refuse("(e) the element is built by emplace from an argument of another type than the container's");

    // The substitute starts open: every constructor must start the flag
    // the waits read as false, by a constant, and nothing else.
    if (c.stop)
        for (cir::FuncOp f : c.ctors) {
            auto inits = c.flagInits.lookup(f);
            bool set = false, other = false;
            for (auto &[m, v] : inits)
                if (m == *c.stop) (v && !*v ? set : other) = true;
            if (other || !set)
                c.refuse("(d) " + shortName(f) + " starts the stop flag " + c.memberName(*c.stop) +
                         (other ? " other than false" : " indeterminate") + ": the substitute starts open");
        }

    // A pop that waits must be woken by every push, and a push that waits
    // for room by every pop.
    auto isPush = [](Role r) {
        return r == Role::Push || r == Role::PushWait || r == Role::TryPush || r == Role::PushOverrun;
    };
    auto isPop = [](Role r) {
        return r == Role::TryPop || r == Role::PopWait || r == Role::PopWaitClosed || r == Role::PopWaitFor;
    };
    std::optional<unsigned> dataCv, spaceCv;
    bool waits = false;
    for (const Method &m : c.methods)
        if (m.role == Role::PopWait || m.role == Role::PopWaitClosed || m.role == Role::PopWaitFor) {
            waits = true;
            if (dataCv && *dataCv != m.waitCv)
                c.refuse("(d) the pops wait on two condition variables");
            dataCv = m.waitCv;
        } else if (m.role == Role::PushWait) {
            if (spaceCv && *spaceCv != m.waitCv)
                c.refuse("(d) the pushes wait on two condition variables");
            spaceCv = m.waitCv;
        }
    for (const Method &m : c.methods) {
        if (!c.reasons.empty()) break;
        if (isPush(m.role) && waits &&
            llvm::none_of(m.notifies, [&](auto n) { return n.first == *dataCv; }))
            c.refuse("(d) " + shortName(m.f) + " does not notify " + c.memberName(*dataCv) +
                     ", which a pop waits on");
        if (isPop(m.role) && spaceCv &&
            llvm::none_of(m.notifies, [&](auto n) { return n.first == *spaceCv; }))
            c.refuse("(d) " + shortName(m.f) + " does not notify " + c.memberName(*spaceCv) +
                     ", which a push waits on for room");
        if (m.role == Role::Close) {
            if (waits && llvm::none_of(m.notifies, [&](auto n) { return n.first == *dataCv && n.second; }))
                c.refuse("(d) " + shortName(m.f) + " sets the stop flag but does not wake every waiter on " +
                         c.memberName(*dataCv));
            c.features["stop flag"]++;
        }
        if (m.role == Role::PopWaitClosed) c.features["closable pop"]++;
    }
    // A pop that ignores the flag would return, once the queue is closed
    // and empty, where the original waits on.
    const bool closes = llvm::any_of(c.methods, [](const Method &m) { return m.role == Role::Close; });
    for (const Method &m : c.methods) {
        if (closes && m.role == Role::PopWait)
            c.refuse("(d) " + shortName(m.f) + " waits only for an element in a class that also closes");
        // The substitute's timed pop gives up at close() as well; the
        // original's waits for an element until its time is up.
        if (closes && m.role == Role::PopWaitFor)
            c.refuse("(d) " + shortName(m.f) + " waits with a time limit only for an element in a class that also "
                     "closes");
        // The substitute's waiting push gives up at close() while the queue
        // is full; one that ignores the flag would wait on.
        if (closes && m.role == Role::PushWait)
            c.refuse("(d) " + shortName(m.f) + " waits only for room in a class that also closes");
    }
    if (c.kind == Kind::Ring && c.reasons.empty()) boundedChecks(c);
    if (c.methods.empty() && c.reasons.empty()) c.refuse("(a) no member function pushes or pops");
    if (c.ctors.empty() && c.reasons.empty())
        c.refuse("(a) no constructor of " + recordName(c.rec) + " builds the container here");
}

// (f) The class is this module's alone. Lifting changes what its member
// functions do with the object's storage; a copy of them compiled in
// another unit, as written, would read the same storage as the container
// it no longer holds, and the linker keeps one copy of an inline member
// function for the whole program, which may be either. So the class lifts
// only when the module is the whole program as far as the class goes: it
// has main, it is not compiled to be linked with files the pass does not
// see, none of the files it is linked with names the class (a member
// function, or a function that takes or returns the class), and every
// member function of the class this module declares has its body here.
void LiftLockedQueuesPass::closedWorld(Candidate &c) {
    const std::string cls = recordName(c.rec).str();
    if (openWorld) {
        c.refuse("(f) the unit is compiled to be linked with files the pass does not see, which may use " + cls +
                 " as written");
        return;
    }
    if (!hasMain) {
        c.refuse("(f) the module has no main: the rest of the program, elsewhere, may use " + cls +
                 " as written");
        return;
    }
    for (cir::FuncOp f : module.getOps<cir::FuncOp>())
        if (f.getBody().empty() && isMemberOf(f, c.rec))
            c.refuse("(f) " + shortName(f) + " has no body here: another unit defines it");
    const std::string want = squashSpaces(cls);
    for (const std::string &s : elsewhere)
        if (mentions(s, want)) {
            c.refuse("(f) a file linked with this one names " + cls + " (" + s + ")");
            break;
        }
}

// Whether every use of f's symbol is a direct call, and which.
bool LiftLockedQueuesPass::onlyDirectCalls(cir::FuncOp f, llvm::SmallVectorImpl<cir::CallOp> &calls) {
    auto uses = SymbolTable::getSymbolUses(f, module);
    if (!uses) return false;
    for (const SymbolTable::SymbolUse &u : *uses) {
        auto call = dyn_cast<cir::CallOp>(u.getUser());
        if (!call || call.getCalleeAttr() != u.getSymbolRef()) return false;
        calls.push_back(call);
    }
    return true;
}

// Whether what f returns only ever goes back to a caller: returned as it
// is, through the return slot, by every caller and theirs, or dropped. A
// count that is approximate while operations are in flight may be handed
// to the program's user; it may not decide anything in the program.
bool LiftLockedQueuesPass::onlyReturned(cir::FuncOp f, unsigned depth, llvm::DenseSet<Operation *> &seen) {
    if (depth > 16) return false;
    if (!seen.insert(f).second) return true;
    llvm::SmallVector<cir::CallOp, 4> calls;
    if (!onlyDirectCalls(f, calls)) return false;
    for (cir::CallOp call : calls) {
        if (call.getNumResults() == 0) continue;
        auto caller = call->getParentOfType<cir::FuncOp>();
        for (Operation *user : call.getResult().getUsers()) {
            if (isa<cir::ReturnOp>(user)) {
                if (!onlyReturned(caller, depth + 1, seen)) return false;
                continue;
            }
            auto st = dyn_cast<cir::StoreOp>(user);
            auto slot = st && st.getValue() == call.getResult()
                            ? traceToLocalRoot(st.getAddr()).getDefiningOp<cir::AllocaOp>()
                            : cir::AllocaOp();
            if (!slot) return false;
            for (Operation *su : slot->getUsers()) {
                if (su == st.getOperation() || isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(su)) continue;
                auto ld = dyn_cast<cir::LoadOp>(su);
                if (!ld) return false;
                for (Operation *lu : ld->getUsers())
                    if (!isa<cir::ReturnOp>(lu)) return false;
            }
            if (!onlyReturned(caller, depth + 1, seen)) return false;
        }
    }
    return true;
}

// What a bounded ring needs beyond the shapes: an overrun count that is
// only handed back, and, for every push that overruns, whether it can run.
void LiftLockedQueuesPass::boundedChecks(Candidate &c) {
    // A file linked with this one may call a function of this module, or
    // reach a record it names, with values the module does not show.
    ValueSets::Elsewhere others;
    if (!elsewhere.empty()) {
        others.calls = [this](cir::FuncOp f) {
            return llvm::is_contained(elsewhere, squashSpaces(llvm::demangle(f.getSymName().str())));
        };
        others.names = [this](cir::RecordType r) {
            const std::string want = squashSpaces(recordName(r));
            return llvm::any_of(elsewhere, [&](const std::string &s) { return mentions(s, want); });
        };
    }
    ValueSets values(module, others);
    for (const Method &m : c.methods) {
        if (m.role == Role::OverrunCounter) {
            llvm::DenseSet<Operation *> seen;
            if (!onlyReturned(m.f, 0, seen))
                c.refuse("(d) the overrun count " + shortName(m.f) + " returns is used for more than handing it "
                         "back: the substitute's count is exact only once the pushes in flight have returned");
            else
                c.features["overrun count, handed back only"]++;
        }
        if (m.role != Role::PushOverrun) continue;
        // The overrun path makes its callers consumers; it is left out only
        // where no call of it can run.
        llvm::SmallVector<cir::CallOp, 4> calls;
        std::string why;
        bool reachable = !onlyDirectCalls(m.f, calls);
        if (reachable) why = "its address is taken";
        for (cir::CallOp call : calls) {
            if (reachable) break;
            std::string w;
            if (!values.neverRuns(call, w)) {
                reachable = true;
                why = "the call in " + shortName(call->getParentOfType<cir::FuncOp>()) + " can run: " + w;
            } else if (why.empty()) {
                why = "the call in " + shortName(call->getParentOfType<cir::FuncOp>()) + " never runs: " + w;
            }
        }
        if (calls.empty() && !reachable) why = "nothing calls it";
        c.overrunReachable |= reachable;
        emitRemark(c.loc) << "wtc: locked queue " << c.label() << ": " << shortName(m.f)
                          << (reachable ? " may overrun, so its callers are consumers too ("
                                        : " never runs, so its overrun is left out (")
                          << why << ")";
    }
}

// Constructors build the container with its default constructor and set
// the stop flag false; destructors destroy it. Nothing else of the queue's.
// A bounded ring is built by the constructor that takes its capacity.
bool LiftLockedQueuesPass::constructorOk(Candidate &c, cir::FuncOp f, bool dtor) {
    bool ok = true;
    llvm::ArrayRef<Type> members = c.rec.getMembers();
    f.walk([&](cir::GetMemberOp gm) {
        auto m = memberOfThis(gm.getResult(), f, c.rec);
        if (!m) return;
        const bool ours = *m == c.q || (c.mutex && *m == *c.mutex) || llvm::is_contained(c.cvs, *m) ||
                          isStopType(members[*m]);
        if (!ours) return;
        for (Operation *u : gm->getUsers()) {
            if (auto call = dyn_cast<cir::CallOp>(u)) {
                cir::FuncOp callee = lookupCallee(call);
                Name n = callee ? nameOf(callee.getSymName()) : Name();
                if (*m == c.q && c.kind == Kind::Ring && n.ctor && call.getNumOperands() == 2 &&
                    call.getOperand(0) == gm.getResult() && callee == c.ring->sizedConstructor() && !dtor)
                    continue;
                if (*m == c.q && c.kind == Kind::Ring && n.ctor) {
                    c.refuse("(a) " + shortName(f) + " builds the ring other than with its capacity");
                    ok = false;
                    continue;
                }
                if (call.getNumOperands() == 1 && call.getOperand(0) == gm.getResult() && (n.ctor || n.dtor)) {
                    if (*m == c.q && n.ctor && dtor) {
                        c.refuse("(a) " + shortName(f) + " builds " + c.memberName(*m) + " in a destructor");
                        ok = false;
                    }
                    // std::atomic<bool>'s default constructor leaves the
                    // value indeterminate before C++20.
                    if (isStopType(members[*m]) && n.ctor && !dtor) c.flagInits[f].push_back({*m, std::nullopt});
                    continue;
                }
                if (isStopType(members[*m]) && n.ctor && call.getNumOperands() == 2) {
                    auto k = call.getOperand(1).getDefiningOp<cir::ConstantOp>();
                    auto b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
                    if (!dtor) c.flagInits[f].push_back({*m, b ? std::optional<bool>(b.getValue()) : std::nullopt});
                    continue;
                }
                if (isStopType(members[*m])) continue;  // judged only if it turns out to be the flag
                c.refuse("(a) " + shortName(f) + " calls " + (callee ? pretty(callee.getSymName()) : "something") +
                         " on " + c.memberName(*m));
                ok = false;
                continue;
            }
            if (auto st = dyn_cast<cir::StoreOp>(u); st && st.getAddr() == gm.getResult() &&
                                                      isStopType(members[*m])) {
                auto k = st.getValue().getDefiningOp<cir::ConstantOp>();
                auto b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
                c.flagInits[f].push_back({*m, b ? std::optional<bool>(b.getValue()) : std::nullopt});
                continue;
            }
            if (isStopType(members[*m])) continue;
            c.refuse("(a) " + shortName(f) + " uses " + c.memberName(*m) + " other than to build or destroy it");
            ok = false;
        }
    });
    return ok;
}

void LiftLockedQueuesPass::decide(Candidate &c, Reader &r, cir::FuncOp f, bool explained) {
    const std::string fn = shortName(f);
    if (!r.why.empty()) {
        // A function the walk already gave a reason for needs no second one.
        if (!explained) c.refuse("(a) " + fn + " " + r.why);
        return;
    }
    Method m;
    m.f = f;
    llvm::SmallVector<const Stmt *, 8> q;  // the container's statements, in order
    const Stmt *lastReturn = nullptr;
    const Stmt *flagIf = nullptr;
    for (const Stmt &s : r.stmts) {
        if (s.k == Stmt::Notify) {
            // Under the original's mutex, a thread that sees what the
            // operation did (takes the element, finds the queue closed)
            // sees it only after the notification has returned, and may
            // destroy the queue at once. The substitute wakes its sleepers
            // after the element is visible, so such a thread could destroy
            // it while the wake-up still runs. Notified after the unlock,
            // as usual, the original already touches the condition variable
            // after that point, and a program may not destroy it there.
            if (s.held) {
                c.refuse("(d) " + fn + " notifies " + c.memberName(s.cv) + " with the mutex held: a thread that sees "
                         "its effect may destroy the queue while the substitute is still waking a sleeper");
                return;
            }
            m.notifies.push_back({s.cv, s.all});
            continue;
        }
        if (s.k == Stmt::FlagIf) {
            flagIf = &s;
            continue;
        }
        if (s.k == Stmt::Return) {
            lastReturn = &s;
            if (s.held) q.push_back(&s);
            continue;
        }
        q.push_back(&s);
    }
    // One critical section holds every touch of the container.
    unsigned cs = 0;
    for (const Stmt *s : q) {
        if (s->k == Stmt::Return) continue;
        if (!s->held) {
            c.refuse("(b) " + fn + " touches " + c.memberName(c.q) + " without the mutex held");
            return;
        }
        if (cs && s->cs != cs) {
            c.refuse("(b) " + fn + " lets the mutex go between two touches of " + c.memberName(c.q));
            return;
        }
        cs = s->cs;
    }
    auto kinds = [&]() {
        llvm::SmallVector<Stmt::K, 8> k;
        for (const Stmt *s : q)
            if (s->k != Stmt::Return) k.push_back(s->k);
        return k;
    }();
    // A flag set in anything but a close is state the mutex guards.
    if (!(kinds.size() == 1 && kinds.front() == Stmt::SetStop))
        for (const Stmt *s : q)
            if (s->k == Stmt::SetStop) {
                c.refuse("(c) the mutex " + c.memberName(*c.mutex) + " also guards " + c.memberName(s->param) +
                         " in " + fn);
                return;
            }
    using K = Stmt::K;
    auto is = [&](std::initializer_list<K> want) {
        return kinds.size() == want.size() && std::equal(want.begin(), want.end(), kinds.begin());
    };
    auto returnsTrue = [&]() { return lastReturn && lastReturn->value && *lastReturn->value; };
    auto outOf = [&](const Stmt *front, const Stmt *out) {
        return out->front == traceToLocalRoot(front->front);
    };
    const bool returnsBool = f.getFunctionType().getReturnTypes().size() == 1;
    const bool ring = c.kind == Kind::Ring;
    auto takePush = [&](const Stmt *p) {
        m.move = p->move;
        m.param = p->param;
        m.byValueSlot = p->byValueSlot;
    };

    if (ring && is({K::Wait, K::Push})) {
        const Stmt *w = q[0];
        if (w->table != kNotFull) {
            c.refuse("(d) " + fn + " waits for something other than \"not full\" before it pushes");
            return;
        }
        if (returnsBool) {
            c.refuse("(a) " + fn + " waits for room, pushes and returns a value");
            return;
        }
        m.role = Role::PushWait;
        takePush(q[1]);
        m.waitCv = w->cv;
    } else if (ring && is({K::GuardedPush})) {
        const Stmt *g = q[0];
        if (g->table != kNotFull) {
            c.refuse("(d) " + fn + " pushes under a test other than \"not full\"");
            return;
        }
        if (returnsBool || (g->flag && (!flagIf || flagIf->flag != g->flag))) {
            c.refuse("(a) " + fn + " pushes where there is room in a form other than a push that may decline");
            return;
        }
        m.role = Role::TryPush;
        takePush(g);
        m.section = g->section;
        m.pushedFlag = g->flag;
        if (flagIf)
            flagIf->op->walk([&](cir::CallOp call) {
                for (const Stmt &n : r.stmts)
                    if (n.k == Stmt::Notify && n.op == call.getOperation()) m.notifyOps.push_back(call);
            });
    } else if (ring && is({K::Push})) {
        // The ring's push_back overruns the oldest when it is full.
        if (returnsBool) {
            c.refuse("(a) " + fn + " pushes and returns a value");
            return;
        }
        m.role = Role::PushOverrun;
        takePush(q[0]);
    } else if (ring && is({K::TimedWait, K::TimedOut, K::Front, K::Out, K::PopFront})) {
        const Stmt *w = q[0], *fr = q[2], *o = q[3];
        if (w->table != kNotEmpty) {
            c.refuse("(d) " + fn + " waits with a time limit for something other than \"not empty\"");
            return;
        }
        if (!returnsBool || !returnsTrue() || !outOf(fr, o)) {
            c.refuse("(a) " + fn + " takes the front in a form other than a pop with a time limit");
            return;
        }
        m.role = Role::PopWaitFor;
        m.move = o->move;
        m.param = o->param;
        m.waitCv = w->cv;
        m.durationParam = w->durationParam;
        m.unit = w->unit;
    } else if (ring && is({K::Getter})) {
        if (!lastReturn || !lastReturn->fromGetter) {
            c.refuse("(d) " + fn + " reads the ring's " + ringOpName(q[0]->getter) + " for something other than "
                     "returning it");
            return;
        }
        m.role = q[0]->getter == RingOp::Size ? Role::Size : Role::OverrunCounter;
    } else if (ring && is({K::Reset})) {
        if (returnsBool) {
            c.refuse("(a) " + fn + " resets the overrun count and returns a value");
            return;
        }
        m.role = Role::ResetOverrun;
    } else if (is({K::Push})) {
        const Stmt *p = q.front();
        m.role = Role::Push;
        m.move = p->move;
        m.param = p->param;
        m.byValueSlot = p->byValueSlot;
        if (returnsBool) {
            if (!returnsTrue()) {
                c.refuse("(a) " + fn + " pushes and returns something other than true");
                return;
            }
            m.pushReturns = true;
        }
    } else if (is({K::Guard, K::Front, K::Out, K::PopFront})) {
        const Stmt *g = q[0], *fr = q[1], *o = q[2];
        if (g->table != kEmpty || !g->value || *g->value || !returnsTrue() || !outOf(fr, o)) {
            c.refuse("(a) " + fn + " checks and takes the front in a form other than try-pop");
            return;
        }
        m.role = Role::TryPop;
        m.move = o->move;
        m.param = o->param;
    } else if (is({K::Wait, K::Front, K::Out, K::PopFront})) {
        const Stmt *w = q[0], *fr = q[1], *o = q[2];
        if (w->table != kNotEmpty) {
            if ((w->table & kNotEmptyOrStop) == kNotEmptyOrStop)
                c.refuse("(d) " + fn + " waits for the stop flag and then takes the front whether or not the "
                         "queue is empty");
            else
                c.refuse("(d) " + fn + " waits for something other than \"not empty\"");
            return;
        }
        if (!outOf(fr, o) || returnsBool) {
            c.refuse("(a) " + fn + " takes the front in a form other than a blocking pop");
            return;
        }
        m.role = Role::PopWait;
        m.move = o->move;
        m.param = o->param;
        m.waitCv = w->cv;
    } else if (is({K::Wait, K::Guard, K::Front, K::Out, K::PopFront})) {
        const Stmt *w = q[0], *g = q[1], *fr = q[2], *o = q[3];
        if (w->table != kNotEmptyOrStop) {
            c.refuse("(d) " + fn + " waits for something other than \"not empty or stopped\"");
            return;
        }
        // After the wait, `empty` and `stopped and empty` are one test.
        if ((g->table & kAfterClosableWait) != (kEmpty & kAfterClosableWait)) {
            if (g->table & bitOf(false, true))
                c.refuse("(d) " + fn + " returns at the stop flag with elements still queued: discarding on "
                         "stop has no faithful equivalent yet");
            else
                c.refuse("(d) " + fn + " decides after the wait on something other than emptiness");
            return;
        }
        if (!g->value || *g->value || !returnsTrue() || !outOf(fr, o)) {
            c.refuse("(a) " + fn + " takes the front in a form other than a closable pop");
            return;
        }
        m.role = Role::PopWaitClosed;
        m.move = o->move;
        m.param = o->param;
        m.waitCv = w->cv;
    } else if (is({K::SetStop})) {
        if (returnsBool) {
            c.refuse("(a) " + fn + " sets the stop flag and returns a value");
            return;
        }
        m.role = Role::Close;
        for (const Stmt *s : q)
            if (s->k == Stmt::SetStop) m.flag = s->param;
    } else {
        std::string seq;
        for (K k : kinds) {
            static const char *names[] = {"push", "front", "take", "pop", "wait", "check", "notify", "stop", "return",
                                          "push if room", "after a push that may decline", "timed wait",
                                          "return at the time limit", "count", "reset"};
            if (!seq.empty()) seq += ", ";
            seq += names[k];
        }
        c.refuse("(a) " + fn + " is none of push, try-pop, blocking pop, closable pop and close (" +
                 (seq.empty() ? std::string("nothing") : seq) + ")");
        return;
    }
    c.methods.push_back(m);
}

// The functions of wtc::lifted<C> in this module, by their annotation.
llvm::StringMap<cir::FuncOp> LiftLockedQueuesPass::runtimeFor(Candidate &c) {
    llvm::StringMap<cir::FuncOp> rt;
    Type q = c.rec.getElementType(c.q);
    for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
        auto ann = f.getAnnotationsAttr();
        if (!ann || f.getBody().empty()) continue;
        auto args = f.getArgumentTypes();
        auto pt = args.empty() ? cir::PointerType() : dyn_cast<cir::PointerType>(args[0]);
        if (!pt || pt.getPointee() != q) continue;
        for (Attribute a : ann)
            if (auto an = dyn_cast<cir::AnnotationAttr>(a); an && an.getName().getValue().starts_with("wtc_lifted_"))
                rt[an.getName().getValue().drop_front(strlen("wtc_lifted_"))] = f;
    }
    return rt;
}

// The constant a function of the runtime returns: `return <bool>;`.
std::optional<bool> constantResult(cir::FuncOp f) {
    std::optional<bool> r;
    bool other = false;
    f.walk([&](cir::ReturnOp ret) {
        if (ret.getNumOperands() != 1) return (void)(other = true);
        Value v = ret.getOperand(0);
        if (auto ld = v.getDefiningOp<cir::LoadOp>()) {
            // -O0: the constant goes through the retval slot.
            for (Operation *u : traceToLocalRoot(ld.getAddr()).getUsers())
                if (auto st = dyn_cast<cir::StoreOp>(u); st && st.getAddr() == ld.getAddr()) v = st.getValue();
        }
        auto k = v.getDefiningOp<cir::ConstantOp>();
        auto b = k ? dyn_cast<cir::BoolAttr>(k.getValue()) : cir::BoolAttr();
        if (!b || (r && *r != b.getValue())) return (void)(other = true);
        r = b.getValue();
    });
    return other ? std::nullopt : r;
}

void LiftLockedQueuesPass::lift(Candidate &c, llvm::StringMap<cir::FuncOp> &rt) {
    MLIRContext *ctx = &getContext();
    Type qType = c.rec.getElementType(c.q);
    auto qPtr = cir::PointerType::get(qType);
    auto call = [&](OpBuilder &b, Location loc, StringRef which, ValueRange args) -> Value {
        cir::FuncOp f = rt.lookup(which);
        auto results = f.getFunctionType().getReturnTypes();
        auto op = cir::CallOp::create(b, loc, FlatSymbolRefAttr::get(f.getSymNameAttr()),
                                      results.empty() ? Type() : results.front(), args);
        return results.empty() ? Value() : op.getResult();
    };
    const bool ring = c.kind == Kind::Ring;
    // The runtime function each method becomes.
    auto runtimeOf = [&](const Method &m) -> std::string {
        const char *how = m.move ? "_move" : "_copy";
        switch (m.role) {
        case Role::Push: return std::string("push") + how;
        case Role::PushWait: return std::string("push_wait") + how;
        case Role::TryPush: return std::string("try_push") + how;
        case Role::PushOverrun:
            return std::string(c.overrunReachable ? "push_overrun" : "push_overrun_unreachable") + how;
        case Role::TryPop: return "try_pop_move";
        case Role::PopWait:
        case Role::PopWaitClosed: return "pop_wait_move";
        case Role::PopWaitFor: return "pop_wait_for_" + m.unit;
        case Role::Close: return "close";
        case Role::Size: return "size";
        case Role::OverrunCounter: return "overrun_counter";
        case Role::ResetOverrun: return "reset_overrun_counter";
        case Role::None: break;
        }
        return "";
    };
    llvm::SmallVector<std::string, 8> needs{ring ? "construct_bounded" : "construct", "destroy", "copy_ok"};
    for (const Method &m : c.methods) needs.push_back(runtimeOf(m));
    for (const std::string &need : needs)
        if (!rt.lookup(need)) {
            emitRemark(c.loc) << "wtc: locked queue " << c.label() << " not lifted: wtc::lifted"
                              << (ring ? "_bounded" : "") << " lacks " << need;
            return;
        }
    // The ring's capacity goes to the runtime as it is.
    if (ring && rt.lookup("construct_bounded").getArgumentTypes()[2] !=
                    c.ring->sizedConstructor().getArgumentTypes()[1]) {
        emitRemark(c.loc) << "wtc: locked queue " << c.label() << " not lifted: the ring's capacity is not a size_t";
        return;
    }
    // A timed pop passes the program's duration on as it is: the runtime's
    // must be the same type.
    for (const Method &m : c.methods)
        if (m.role == Role::PopWaitFor &&
            rt.lookup(runtimeOf(m)).getArgumentTypes()[2] != cir::FuncOp(m.f).getArgumentTypes()[m.durationParam]) {
            emitRemark(c.loc) << "wtc: locked queue " << c.label() << " not lifted: " << shortName(m.f)
                              << "'s duration is not the type wtc::lifted_bounded takes";
            return;
        }
    // What the runtime offers for this element type.
    const bool copyOk = constantResult(rt.lookup("copy_ok")).value_or(false);
    bool refused = false;
    for (const Method &m : c.methods) {
        const bool takes = m.role == Role::TryPop || m.role == Role::PopWait || m.role == Role::PopWaitClosed;
        if (takes && !m.move && !copyOk) {
            emitRemark(c.loc) << "wtc: locked queue " << c.label() << " not lifted: (e) " << shortName(m.f)
                              << " copies the element out, and wtc::queue moves it: the same only for a "
                                 "trivially copyable type";
            refused = true;
        }
    }
    if (refused) return;

    // Constructors and destructors: the container's own calls become the
    // runtime's.
    for (cir::FuncOp f : llvm::concat<cir::FuncOp>(c.ctors, c.dtors)) {
        llvm::SmallVector<cir::CallOp, 2> calls;
        f.walk([&](cir::CallOp op) {
            if (op.getNumOperands() != 1) return;
            auto m = memberOfThis(op.getOperand(0), f, c.rec);
            if (m && *m == c.q) calls.push_back(op);
        });
        if (ring) {
            // The sized constructor, `ring(capacity)`.
            f.walk([&](cir::CallOp op) {
                if (op.getNumOperands() != 2) return;
                auto m = memberOfThis(op.getOperand(0), f, c.rec);
                if (m && *m == c.q && lookupCallee(op) == c.ring->sizedConstructor()) calls.push_back(op);
            });
        }
        for (cir::CallOp op : calls) {
            Name n = nameOf(lookupCallee(op).getSymName());
            OpBuilder b(op);
            if (n.ctor && ring) {
                cir::FuncOp ctor = rt.lookup("construct_bounded");
                Type tagTy = ctor.getArgumentTypes()[1];
                auto tag = cir::ConstantOp::create(b, op.getLoc(), tagTy, cir::IntAttr::get(tagTy, 0));
                auto boolTy = cir::BoolType::get(ctx);
                auto overrun =
                    cir::ConstantOp::create(b, op.getLoc(), boolTy, cir::BoolAttr::get(ctx, boolTy, c.overrunReachable));
                Value cap = op.getOperand(1);
                call(b, op.getLoc(), "construct_bounded", {op.getOperand(0), tag.getResult(), cap, overrun.getResult()});
            } else if (n.ctor) {
                cir::FuncOp ctor = rt.lookup("construct");
                Type tagTy = ctor.getArgumentTypes()[1];
                auto tag = cir::ConstantOp::create(b, op.getLoc(), tagTy, cir::IntAttr::get(tagTy, 0));
                call(b, op.getLoc(), "construct", {op.getOperand(0), tag.getResult()});
            } else {
                call(b, op.getLoc(), "destroy", {op.getOperand(0)});
            }
            op.erase();
        }
    }

    for (const Method &m : c.methods) {
        cir::FuncOp f = m.f;
        Location loc = f.getLoc();
        if (m.role == Role::TryPush) {
            // In place: the critical section becomes the runtime's try_push,
            // whose answer goes to the flag; the notifications after it are
            // the runtime's. The program's code for a declined push stays.
            OpBuilder b(m.section);
            Value self = f.getBody().front().getArgument(thisIndex(f));
            Value at = cir::GetMemberOp::create(b, loc, qPtr, self, c.memberName(c.q), c.q).getResult();
            Value v = f.getBody().front().getArgument(m.param);
            if (m.byValueSlot) {
                auto slot = cir::AllocaOp::create(b, loc, cir::PointerType::get(v.getType()), "lifted.value",
                                                  b.getI64IntegerAttr(8));
                cir::StoreOp::create(b, loc, v, slot.getResult());
                v = slot.getResult();
            }
            Value ok = call(b, loc, runtimeOf(m), {at, v});
            if (m.pushedFlag) cir::StoreOp::create(b, loc, ok, m.pushedFlag);
            for (Operation *n : m.notifyOps) n->erase();
            m.section->erase();
            continue;
        }
        Region &body = f.getBody();
        body.dropAllReferences();
        body.getBlocks().clear();
        Block *entry = new Block();
        body.push_back(entry);
        for (Type t : f.getArgumentTypes()) entry->addArgument(t, loc);
        OpBuilder b = OpBuilder::atBlockBegin(entry);
        Value self = entry->getArgument(0);
        Value at = cir::GetMemberOp::create(b, loc, qPtr, self, c.memberName(c.q), c.q).getResult();
        Value arg = entry->getArgument(m.param);
        auto boolTy = cir::BoolType::get(ctx);
        auto ret = [&](Value v) {
            if (v) cir::ReturnOp::create(b, loc, ValueRange{v});
            else cir::ReturnOp::create(b, loc, ValueRange{});
        };
        switch (m.role) {
        case Role::PushWait:
        case Role::PushOverrun:
        case Role::Push: {
            Value v = arg;
            if (m.byValueSlot) {
                auto slot = cir::AllocaOp::create(b, loc, cir::PointerType::get(arg.getType()), "lifted.value",
                                                  b.getI64IntegerAttr(8));
                cir::StoreOp::create(b, loc, arg, slot.getResult());
                v = slot.getResult();
            }
            call(b, loc, runtimeOf(m), {at, v});
            if (m.pushReturns) {
                auto t = cir::ConstantOp::create(b, loc, boolTy, cir::BoolAttr::get(ctx, boolTy, *m.pushReturns));
                ret(t.getResult());
            } else {
                ret(Value());
            }
            break;
        }
        case Role::TryPop:
            ret(call(b, loc, "try_pop_move", {at, arg}));
            break;
        case Role::PopWait:
            call(b, loc, "pop_wait_move", {at, arg});  // never false: nothing closes it
            ret(Value());
            break;
        case Role::PopWaitClosed:
            ret(call(b, loc, "pop_wait_move", {at, arg}));
            break;
        case Role::Close:
            call(b, loc, "close", {at});
            ret(Value());
            break;
        case Role::PopWaitFor:
            ret(call(b, loc, runtimeOf(m), {at, arg, entry->getArgument(m.durationParam)}));
            break;
        case Role::Size:
        case Role::OverrunCounter:
            ret(call(b, loc, runtimeOf(m), {at}));
            break;
        case Role::ResetOverrun:
            call(b, loc, runtimeOf(m), {at});
            ret(Value());
            break;
        case Role::TryPush:
        case Role::None:
            break;
        }
    }
    InFlightDiagnostic d = emitRemark(c.loc) << "wtc: locked queue " << c.label() << " lifted (";
    llvm::interleaveComma(c.methods, d, [&](const Method &m) { d << shortName(m.f) << ": " << roleName(m.role); });
    d << ")";
}

// The runtime's functions nothing calls any more: those of classes that
// did not lift, and the forms a lifted class does not use. Left in place,
// the pushes and pops inside them would be lifted as operations on queues
// nothing builds.
void LiftLockedQueuesPass::eraseUnusedRuntime() {
    bool changed = true;
    while (changed) {
        changed = false;
        for (cir::FuncOp f : llvm::make_early_inc_range(module.getOps<cir::FuncOp>())) {
            auto ann = f.getAnnotationsAttr();
            if (!ann) continue;
            bool runtime = llvm::any_of(ann, [](Attribute a) {
                auto an = dyn_cast<cir::AnnotationAttr>(a);
                return an && an.getName().getValue().starts_with("wtc_lifted_");
            });
            if (!runtime || !SymbolTable::symbolKnownUseEmpty(f, module)) continue;
            f.erase();
            changed = true;
        }
    }
}

} // namespace

namespace mlir::wtc {
void registerLiftLockedQueuesPass() { PassRegistration<LiftLockedQueuesPass>(); }
}
