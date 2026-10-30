#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/ThreadInstances.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <functional>
#include <optional>

// For every queue object the module touches, how many threads push to it
// and how many pop from it, as a verdict written into the IR:
//
//   #wtc.topology<spsc>   one producer thread, one consumer thread
//   #wtc.topology<mpsc>   ... and so on
//   #wtc.topology<unknown> the analysis could not be sure
//
// The rule that makes the verdict usable for substitution is that every
// unresolved case maps to unknown, never to a smaller count. Substituting a
// single-producer structure into a program with two producers does not run
// slower, it races; so the analysis over-approximates threads and never
// under-approximates them.
//
// The walk over dynamic thread and function instances, which is the subtle
// part and is shared with every other analysis here, is in ThreadInstances.
//
// A queue is a place the walk names: a root (a global, a stack slot, or the
// allocation call that made a heap object) and how many bytes into it the
// queue starts. A queue that is the whole of a global or a slot carries its
// verdict on that root. One that is a member of a struct or a heap object
// carries it on the one constructor call that builds it, when one call
// builds it and nothing else; a constructor shared with another queue -- a
// struct's, run for two objects -- has a single tag for both, and both keep
// the general-purpose implementation. All objects one allocation site makes
// are one queue to the analysis: the threads of each are among the threads
// of all, so spsc for all is spsc for each.
//
// A program can also state the topology itself, by passing a tag other than
// generic to the queue's constructor. That is a claim, and it is checked:
// when the threads that use the queue certainly break it -- two threads the
// module accounts for push to it, or one that certainly stands for many --
// the build fails, naming each thread and where it touches the queue; when
// the analysis cannot tell, a warning says why; when it agrees, a remark
// says the declaration is verified.
//
// The analysis sees one translation unit. What it assumes of the rest of
// the program, and what it checks (docs/audit.md, "Still open"): a unit
// that defines main is the program, apart from libraries, which reach the
// program's queues only through what they are handed. A unit without
// main, or one built to be linked with other files of the program
// (open-world; or named-elsewhere, the symbols those files name, which
// also stands for the rest of a unit without main), shares its external
// symbols: a function another unit may call is a root for
// any number of threads, and a queue another unit may name -- one in, or
// reachable from, a global it may name, or handed out by a function it may
// call -- escapes. A queue whose address goes to a function with no body
// here escapes in either case. Every queue of an escaping queue's type is
// unknown, and keeps the general-purpose implementation.

namespace {

// One access of a queue by one thread, kept for the declared-topology
// check, which has to name the threads it finds.
struct Access {
    Operation *op;
    unsigned thread;
    bool many;          // the thread certainly stands for several
    bool speculative;   // a thread the module cannot account for
    std::string entry;  // the function the thread starts in, demangled
    Location started;   // the spawn that starts it, or the entry for main
    bool spawned;
    bool oneOf = false;  // the operation may act on this queue or another
    const Instance *inst = nullptr;   // where the walk saw it, while the walk lives
    // The counted loop this access is credited to: the thread was started
    // by one run of that loop, and that run belongs to the constructor of
    // the object the access touches (see SideInfo::countLoop). Null when
    // the thread was started some other way, or by the loop of another
    // object's constructor.
    Operation *countLoop = nullptr;
    // For a put into some pool the analysis does not name -- a free of an
    // mbuf whose pool is its `pool` field, unresolved -- why; counted on
    // every pool that may hold mbufs, as one of several (oneOf).
    std::string poolWhy{};
};

struct SideInfo {
    llvm::SmallSetVector<unsigned, 4> threads;
    llvm::SmallVector<Access, 4> accesses;
    bool anyMany = false;
    // 0 if nobody touches this side, 2 for "many", else 1.
    unsigned count() const {
        if (threads.empty()) return 0;
        return (anyMany || threads.size() >= 2) ? 2 : 1;
    }
    // The counted loop that starts every thread on this side, at most one
    // per iteration, when there is one: the side then has one thread at
    // most when the loop runs at most once. The walk may tell several threads
    // apart under one such loop -- a vector's emplace_back constructs the
    // thread on one of two paths -- and together they are still one per
    // iteration, which the loop's record on each instance vouches for.
    //
    // The record is read where the queue is built, as "this object's loop
    // ran at most once", so a thread counts only when the run of the loop
    // that started it is the constructor run of the object it touches:
    // a sink built with no workers, fed by the workers of a source built
    // by the same constructor, has producers started by a loop that ran
    // twice while its own ran not at all. Null when any access is not so
    // credited, is on a thread the module cannot account for, or may be
    // to another queue.
    Operation *countLoop() const {
        Operation *loop = nullptr;
        for (const Access &a : accesses) {
            if (!a.countLoop || a.speculative || a.oneOf) return nullptr;
            if (loop && loop != a.countLoop) return nullptr;
            loop = a.countLoop;
        }
        return loop;
    }
};

struct RootInfo {
    SideInfo producers, consumers;
};

// A queue: the root the walk names, and the queue's offset in it. Or, with
// the offset below, a member of every object of one record type, keyed by
// the constructor call that builds that member (see AbstractQueue).
using Key = std::pair<Operation *, uint64_t>;
constexpr uint64_t kAbstractOffset = ~uint64_t(0);
// With this offset, every element of an array of queues: a global, a slot
// or a heap block whose elements some access or constructor reaches with an
// index the walk cannot read (`&qs[qid]`, qid computed by the thread). The
// elements are one queue to the analysis, as the objects of one allocation
// site are: each element's threads are among the threads of all, so a
// verdict over the union holds for each, and the one constructor call that
// builds the elements, run once per element, carries it to all of them.
// What is lost is precision: two elements with one producer each count two.
constexpr uint64_t kElementsOffset = ~uint64_t(0) - 1;

// Whether `t` is an array, of arrays at any depth, of `elem`.
static bool arrayOf(Type t, Type elem) {
    bool array = false;
    while (auto a = dyn_cast_or_null<cir::ArrayType>(t)) {
        t = a.getElementType();
        array = true;
    }
    return array && elem && t == elem;
}

// A record of DPDK's of the given name.
static bool isRecordNamed(Type t, llvm::StringRef name) {
    auto rec = dyn_cast_or_null<cir::RecordType>(t);
    return rec && rec.getName() && rec.getName().getValue() == name;
}

// The pool type an mbuf's `pool` field points to (struct rte_mempool), for
// an operand that points to an mbuf or to an array of pointers to them;
// null for anything else. The field is found by its type, at any depth of
// the anonymous unions and structures DPDK nests in struct rte_mbuf.
static Type poolMemberType(Type t, unsigned depth) {
    auto rec = dyn_cast_or_null<cir::RecordType>(t);
    if (!rec || rec.isIncomplete() || depth > 4) return Type();
    for (Type m : rec.getMembers()) {
        if (auto p = dyn_cast<cir::PointerType>(m); p && isRecordNamed(p.getPointee(), "rte_mempool"))
            return p.getPointee();
        if (Type inner = isa<cir::PointerType>(m) ? Type() : poolMemberType(m, depth + 1)) return inner;
    }
    return Type();
}
static Type mbufPoolType(Type operand) {
    auto p = dyn_cast<cir::PointerType>(operand);
    Type t = p ? p.getPointee() : Type();
    if (auto pp = dyn_cast_or_null<cir::PointerType>(t)) t = pp.getPointee();
    return isRecordNamed(t, "rte_mbuf") ? poolMemberType(t, 0) : Type();
}
static bool isMbufPointer(Type t) {
    auto p = dyn_cast<cir::PointerType>(t);
    Type e = p ? p.getPointee() : Type();
    if (auto pp = dyn_cast_or_null<cir::PointerType>(e)) e = pp.getPointee();
    return isRecordNamed(e, "rte_mbuf");
}

static bool isNullPointer(Value v);
static bool firstArgIsSret(cir::FuncOp f);
static std::optional<int64_t> constantInteger(Value v, unsigned depth);

// A queue the walk cannot place but can name by type: a member at a known
// offset of a record, reached through a pointer the walk cannot follow --
// `this->q_` in a worker whose `this` came through a tuple or a shared_ptr.
// Every runtime object of the record is one of the objects the walk placed
// or "some object of the record", so the member's threads over all objects
// are at least each object's, and a verdict over the union holds for each.
// The verdict is carried by the one constructor call that builds the
// member, and the tag it is rewritten to applies to every object, which
// the union covers. What is lost is precision: two objects with one thread
// each are counted together.
struct AbstractQueue {
    Type record;
    uint64_t offset;
    std::string member;
};

class QueueOwnershipAnalysisPass
    : public PassWrapper<QueueOwnershipAnalysisPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(QueueOwnershipAnalysisPass)

    QueueOwnershipAnalysisPass() = default;
    QueueOwnershipAnalysisPass(const QueueOwnershipAnalysisPass &other) : PassWrapper(other) {}

    StringRef getArgument() const final { return "queue-ownership-analysis"; }
    StringRef getDescription() const final {
        return "Count the threads that push to and pop from each wtc queue and write the "
               "verdict into the IR as #wtc.topology; unresolved cases yield unknown";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override;

private:
    ModuleOp module;
    llvm::MapVector<Key, RootInfo> roots;          // in discovery order
    llvm::DenseSet<Type> poisonedTypes;            // queue object types with an unresolvable access
    llvm::DenseSet<Operation *> poisonedOps;       // accesses already reported unresolvable
    llvm::DenseMap<Type, std::string> poisonWhy;   // the first reason, for the declared-topology check
    llvm::DenseMap<Operation *, llvm::SmallSetVector<Key, 2>> opRoots;
    // Which queues each constructor call builds, under every instance of
    // its function, and the reverse; a call whose object the walk cannot
    // name builds something unknown and is no queue's own.
    llvm::DenseMap<Operation *, llvm::SmallSetVector<Key, 2>> ctorKeys;
    llvm::DenseMap<Key, llvm::SmallSetVector<Operation *, 2>> keyCtors;
    llvm::DenseSet<Operation *> ctorUnplaced;
    llvm::DenseMap<Key, Type> keyType;             // the queue type its constructor builds
    llvm::DenseMap<Operation *, int64_t> loopIds;  // the counted spawn loops recorded, numbered
    // (record, offset) -> the queue constructor calls that build that
    // member of the record, and the abstract keys in use.
    llvm::DenseMap<std::pair<Type, uint64_t>, llvm::SmallVector<Operation *, 2>> memberCtors;
    llvm::DenseMap<Key, AbstractQueue> abstractKeys;
    // The arrays of queues counted as one (kElementsOffset), by their keys.
    llvm::DenseSet<Key> elementKeys;
    std::optional<DataLayout> layout;
    // The instance whose run built each placed queue: the instance of the
    // function holding its constructor call. kAmbiguous when two runs
    // placed one key, which is then nobody's own object.
    static const Instance *const kAmbiguous;
    llvm::DenseMap<Key, const Instance *> keyCtorInst;
    // (closure type, field) -> every store into that field in the module;
    // empty when the field's address goes anywhere but a load or a store.
    llvm::DenseMap<std::pair<Type, unsigned>, llvm::SmallVector<cir::StoreOp, 2>> captureStores;
    ThreadInstanceWalk *walk = nullptr;
    bool globalPoison = false;   // an access with no type to poison
    unsigned remarks = 0;

    // What each modelled library function is, asked once per function and
    // per run: the walk asks of every call it visits, and the answer comes
    // from demangling the name.
    llvm::DenseMap<Operation *, LibraryQueueFunction> libraryFunctions;
    LibraryQueueFunction libraryFunction(cir::FuncOp f) {
        if (!f) return {};
        auto [it, fresh] = libraryFunctions.try_emplace(f.getOperation());
        if (fresh) it->second = libraryQueueFunction(f);
        return it->second;
    }
    LibraryQueueCall libraryCall(cir::CallOp call) {
        return libraryQueueCall(call, libraryFunction(lookupCallee(call)));
    }
    bool isLibraryFunction(cir::FuncOp f) { return libraryFunction(f).member; }
    // A modelled push, pop or constructor inside a modelled library's own
    // function: the library's, not the program's. rte_ring_enqueue
    // dispatches to rte_ring_sp_enqueue_bulk_elem on the ring's flags,
    // which promises nothing for the program; moodycamel's blocking queue
    // builds the queue it wraps, which is not a second queue.
    bool inLibraryBody(Operation *op) {
        auto call = dyn_cast<cir::CallOp>(op);
        if (!call || !libraryCall(call)) return false;
        auto in = op->getParentOfType<cir::FuncOp>();
        return in && isLibraryFunction(in);
    }

    // Every operation on a queue. Only a pop consumes; push and the three
    // operations of a batched run are the producer's side. A push or a pop
    // of a modelled library queue is one too, left as the call it is.
    LibraryQueueCall libraryOp(Operation *op) {
        auto call = dyn_cast<cir::CallOp>(op);
        if (!call) return {};
        LibraryQueueCall l = libraryCall(call);
        if (l.role != LibraryQueueRole::Push && l.role != LibraryQueueRole::Pop) return {};
        return l;
    }
    bool isQueueOp(Operation *op) {
        return isa<QueuePushOp, QueuePopOp, QueueBatchBeginOp, QueuePushDeferredOp, QueueBatchEndOp>(op) ||
               libraryOp(op);
    }
    bool isConsumerOp(Operation *op) {
        return isa<QueuePopOp>(op) || libraryOp(op).role == LibraryQueueRole::Pop;
    }
    Value queueOf(Operation *op) {
        if (auto o = dyn_cast<QueuePushOp>(op)) return o.getQueue();
        if (auto o = dyn_cast<QueuePopOp>(op)) return o.getQueue();
        if (auto o = dyn_cast<QueueBatchBeginOp>(op)) return o.getQueue();
        if (auto o = dyn_cast<QueuePushDeferredOp>(op)) return o.getQueue();
        if (auto o = dyn_cast<QueueBatchEndOp>(op)) return o.getQueue();
        LibraryQueueCall l = libraryOp(op);
        // A free's queue is the pool its mbuf was taken from, when the
        // analysis can name it; else the mbuf, which no pool is.
        if (l.viaMbuf != MbufFree::None)
            if (Value pool = mbufPool(op)) return pool;
        return op->getOperand(l.queueOperand);
    }

    static Type queueObjectType(Operation *root) {
        if (auto g = dyn_cast<cir::GlobalOp>(root)) return g.getSymType();
        if (auto a = dyn_cast<cir::AllocaOp>(root)) return a.getAllocaType();
        return Type();
    }

    Type queueObjectTypeOfOp(Operation *op) {
        // A free's queue is a pool, named or not: the type its mbuf's
        // `pool` field points to.
        if (LibraryQueueCall l = libraryOp(op); l.viaMbuf != MbufFree::None)
            return mbufPoolType(op->getOperand(l.queueOperand).getType());
        Value q = queueOf(op);
        if (auto qt = dyn_cast<QueueType>(q.getType())) return qt.getElementType();
        if (auto pt = dyn_cast<cir::PointerType>(q.getType())) return pt.getPointee();
        return Type();
    }

    // A call of the shim's queue constructor, or of a modelled library
    // queue's, whose first operand is the object it builds; or
    // rte_ring_create and rte_mempool_create, whose result is.
    bool isQueueConstructor(Operation *op) {
        auto call = dyn_cast<cir::CallOp>(op);
        if (!call) return false;
        if (getSemanticKind(lookupCallee(call)) == SemanticOpKind::QueueConstruction)
            return call.getNumOperands() > 0 && firstRealOperand(call) == 0 && !insideLiftedRuntime(call);
        LibraryQueueCall l = libraryCall(call);
        return l.role == LibraryQueueRole::Create ||
               (l.role == LibraryQueueRole::Construct && l.queueOperand == 0);
    }
    bool isLibraryCreate(Operation *op) {
        auto call = dyn_cast<cir::CallOp>(op);
        return call && libraryCall(call).role == LibraryQueueRole::Create;
    }
    bool isQueueOpOrConstructor(Operation *op) { return isQueueOp(op) || isQueueConstructor(op); }

    // The whole of a global or a slot of the queue's own type.
    static bool isWhole(const Key &k, Type queueTy) {
        return k.second == 0 && isa<cir::GlobalOp, cir::AllocaOp>(k.first) && queueObjectType(k.first) == queueTy;
    }
    // The queue type at a key: what its constructor builds, or the root's
    // own type for the whole of a global or a slot.
    Type queueTypeOf(const Key &k) const {
        // An array's elements: the type its constructors build -- none when
        // they build queues of different types -- or the array's own.
        if (k.second == kElementsOffset) {
            if (auto it = keyType.find(k); it != keyType.end()) return it->second;
            Type t = queueObjectType(k.first);
            bool array = false;
            while (auto a = dyn_cast_or_null<cir::ArrayType>(t)) { t = a.getElementType(); array = true; }
            return array ? t : Type();
        }
        if (Type t = keyType.lookup(k)) return t;
        return k.second == 0 ? queueObjectType(k.first) : Type();
    }
    // Whether the elements of the array at the root of `ek` are queues of
    // type `queueTy`: a constructor call builds them, or the root is a
    // global or a slot of an array of them.
    bool elementsHold(const Key &ek, Type queueTy) const {
        return (queueTy && keyType.lookup(ek) == queueTy) || arrayOf(queueObjectType(ek.first), queueTy);
    }
    // The one constructor call that builds this queue and nothing else, or
    // null with the reason.
    Operation *carrierOf(const Key &k, const char *&why) const;
    std::string labelOf(const Key &k, bool whole) const;
    // The record member a handle names, when the handle is member steps
    // from a pointer to a record, through the parameters of `inst`'s call
    // chain: the outermost record, the member's offset in it and the
    // innermost member's name. `base` receives the pointer the steps
    // started from and `baseInst` the instance it lives in.
    std::optional<AbstractQueue> memberOf(Value handle, Instance *inst, Value *base = nullptr,
                                          Instance **baseInst = nullptr);
    // Whether the object an access through an abstract key touches is the
    // one the constructor instance `holder` built: the handle's base,
    // followed through the parameters of the chain, is a load of a
    // closure's captured `this`, and every store into that capture in the
    // module stores the `this` of holder's function.
    bool sameObject(Operation *op, Instance *inst, const Instance *holder);
    // The abstract key for an access the walk could not place, when its
    // handle names a member one constructor call builds; registered as a
    // queue of that constructor on first use.
    std::optional<Key> abstractKey(Operation *op, Instance *inst);
    // For an allocation whose result one pointer global holds -- stored
    // there once, by the only store to that global -- the global's name,
    // so that a heap queue is named after the pointer the program uses
    // for it. Computed once.
    llvm::StringRef pointerNaming(Operation *allocation) const;
    mutable std::optional<llvm::DenseMap<Operation *, llvm::StringRef>> pointerNames;

    // An operation is poisoned once: the walk can meet the same recursive
    // function along several call strings. Only an access with no type is
    // everyone's problem; one whose type is already poisoned is not, and
    // treating the second sighting as the first case made every queue in
    // the program unknown.
    void poison(Operation *op, const Twine &why) {
        if (!poisonedOps.insert(op).second) return;
        Type t = queueObjectTypeOfOp(op);
        if (!t) globalPoison = true;
        else if (poisonedTypes.insert(t).second) poisonWhy[t] = why.str();
        op->emitRemark() << "wtc: queue access is unresolvable (" << why
                         << "); every queue of this type is now unknown";
        ++remarks;
    }

    // A recursive function's recurrences past the first are not expanded
    // (ThreadInstances.h): what they reach is walked once more, on the
    // same thread, but under one call string, and a handle that a deeper
    // call binds through the recursion's parameters may name an object the
    // walk never saw. Such an access is unresolvable. One whose handle is
    // fixed below the recursion -- a global, reached through parameters of
    // functions outside the cycle only, as ferret's loader hands
    // `&q_load_seg` to enqueue from the file helper its recursive scan
    // calls, or a pointer a global written once holds -- names the same
    // queue at every depth, on the same thread,
    // and is counted as the walk saw it. Recursion that can start a thread
    // or call through a pointer is not scoped: everything it reaches is
    // unresolvable, as before.
    void poisonReachable(cir::FuncOp f) {
        llvm::SetVector<Operation *> reach;
        llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 4>> callsIn;   // callee -> its calls in reach
        bool spawnsOrIndirect = false;
        llvm::SmallVector<cir::FuncOp, 8> work{f};
        while (!work.empty()) {
            cir::FuncOp cur = work.pop_back_val();
            if (!reach.insert(cur.getOperation())) continue;
            cur.walk([&](cir::CallOp call) {
                cir::FuncOp callee = lookupCallee(call);
                if (!callee) { spawnsOrIndirect = true; return; }
                if (getSemanticKind(callee) == SemanticOpKind::ThreadSpawn) spawnsOrIndirect = true;
                if (callee.getBody().empty()) return;
                callsIn[callee.getOperation()].push_back(call);
                work.push_back(callee);
            });
        }
        // The cycle: the functions reached that reach f back.
        llvm::DenseSet<Operation *> cycle{f.getOperation()};
        for (bool grew = true; grew;) {
            grew = false;
            for (auto &[callee, calls] : callsIn)
                if (cycle.contains(callee))
                    for (cir::CallOp c : calls)
                        if (cycle.insert(c->getParentOfType<cir::FuncOp>().getOperation()).second) grew = true;
        }
        for (Operation *g : reach)
            cast<cir::FuncOp>(g).walk([&](Operation *op) {
                if (!isQueueOp(op) || inLibraryBody(op)) return;
                if (!spawnsOrIndirect && fixedBelow(queueOf(op), cast<cir::FuncOp>(g), cycle, callsIn, 0)) return;
                poison(op, "reached through recursion");
            });
    }
    // Whether a handle in g names one object whatever the recursion above
    // binds: member and constant element steps from a global, from a
    // pointer loaded from a global the program writes once, or from a
    // parameter of a function outside the cycle whose every call reached
    // from the recursion passes such a handle.
    bool fixedBelow(Value v, cir::FuncOp g, const llvm::DenseSet<Operation *> &cycle,
                    const llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 4>> &callsIn,
                    unsigned depth) {
        if (depth > 16) return false;
        Value t = traceToRoot(v);
        for (unsigned steps = 0; steps < 64; ++steps) {
            if (auto gm = t.getDefiningOp<cir::GetMemberOp>()) { t = traceToRoot(gm.getAddr()); continue; }
            Value index, base;
            if (auto ps = t.getDefiningOp<cir::PtrStrideOp>()) { index = ps.getStride(); base = ps.getBase(); }
            else if (auto ge = t.getDefiningOp<cir::GetElementOp>()) { index = ge.getIndex(); base = ge.getBase(); }
            if (!base) break;
            if (!constantInteger(index, 0)) return false;
            t = traceToRoot(base);
        }
        if (t.getDefiningOp<cir::GetGlobalOp>()) return true;
        // A pointer a global holds that the program writes once: the same
        // object at every depth (the port of ferret's `q_load_seg`).
        if (auto ld = t.getDefiningOp<cir::LoadOp>())
            if (auto gg = traceToLocalRoot(ld.getAddr()).getDefiningOp<cir::GetGlobalOp>()) {
                auto g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr());
                return g && walk && walk->singleStoredGlobalValue(g);
            }
        auto arg = dyn_cast<BlockArgument>(t);
        if (!arg || cycle.contains(g.getOperation()) || arg.getOwner() != &g.getBody().front()) return false;
        // Reached only by direct calls: its address goes nowhere else.
        auto uses = SymbolTable::getSymbolUses(g, module);
        if (!uses) return false;
        for (const SymbolTable::SymbolUse &u : *uses) {
            auto call = dyn_cast<cir::CallOp>(u.getUser());
            if (!call || call.getCalleeAttr() != u.getSymbolRef()) return false;
        }
        auto it = callsIn.find(g.getOperation());
        if (it == callsIn.end() || it->second.empty()) return false;
        for (cir::CallOp call : it->second) {
            const unsigned idx = arg.getArgNumber();
            if (idx >= call.getNumOperands()) return false;
            if (!fixedBelow(call.getOperand(idx), call->getParentOfType<cir::FuncOp>(), cycle, callsIn, depth + 1))
                return false;
        }
        return true;
    }

    // A queue's number is its position in discovery order, which follows
    // the walk and is therefore deterministic.
    // Library queues are not numbered: they carry no verdict a later pass
    // reads, and a program's shim queues keep the numbers they had.
    int64_t idOf(const Key &k) const {
        int64_t n = 0;
        for (const auto &entry : roots) {
            if (entry.first == k) return n;
            if (!libraryKeys.contains(entry.first)) ++n;
        }
        return n;
    }

    void writeVerdicts(bool capExceeded);
    // False when a declared topology is certainly broken. A summary queue
    // stands for every object one allocation site makes, and threads on two
    // of them are no proof against either.
    bool checkDeclared(Operation *root, const RootInfo &info, Topology verdict, const std::string &label,
                       llvm::ArrayRef<cir::CallOp> ctors, bool capExceeded, const char *summary, bool library,
                       Type objTy);
    // What a library queue declares on each side: many threads, one, or
    // not known before the run, with what says so.
    enum class Declared { Many, One, Unknown };
    struct LibraryDeclaration {
        Declared producers = Declared::Many, consumers = Declared::Many;
        std::string how;      // what declares a side single
        std::string unknown;  // why a side is not known
    };
    LibraryDeclaration libraryDeclared(llvm::ArrayRef<cir::CallOp> ctors, const RootInfo &info);
    // Why a library queue of this type may be used by code the module does
    // not show, or empty: verdicts on it hold in this translation unit only.
    std::string openWhy(Type t);
    llvm::DenseMap<Type, std::string> openWhyCache;
    // A modelled push or pop whose address the program takes: calls
    // through the pointer are not counted, so every queue of its type is
    // unknown.
    void poisonAddressTakenLibraryOps();
    // Queues of a modelled library (Utils.h, LibraryQueueCall): counted and
    // checked, never rewritten, so no constructor carries a verdict.
    llvm::DenseSet<Key> libraryKeys;
    static Access accessOf(Operation *op, const Instance &inst);
    // The pool a free puts its mbuf into, when the mbuf is, in the function
    // that frees it, what an allocation from a pool handed back, and no
    // segment or buffer of another pool can be linked to it; else null,
    // with why in `why`.
    Value mbufPool(Operation *op, std::string *why = nullptr);
    // Where the program links one mbuf to another -- a chain, an indirect
    // mbuf -- so that a free reaches more than one pool; null if nowhere.
    // Computed once per run.
    Operation *mbufLinkSite();
    std::optional<Operation *> mbufLink;
    // Frees whose pool mbufPool cannot name: each puts into some pool that
    // may hold mbufs, on its thread.
    struct UnresolvedPut {
        Operation *op;
        Instance *inst;
        std::string why;
    };
    llvm::SmallVector<UnresolvedPut, 4> unresolvedPuts;
    // The pools made in this module that may hold mbufs, in the order the
    // walk met their creations.
    llvm::SmallSetVector<Key, 4> mbufPoolKeys;
    // Pools made by rte_mempool_create with no initialiser, which hold
    // mbufs only if one is handed to them later.
    llvm::SmallSetVector<Key, 4> objectPools;
    // Why a pool's put side may hold unresolved frees, or null.
    static const Access *unresolvedPutOf(const SideInfo &side) {
        for (const Access &a : side.accesses)
            if (!a.poolWhy.empty()) return &a;
        return nullptr;
    }
    // What a library queue's creation declares: a side single by bit (1:
    // one producer, 2: one consumer), as the program writes it, or not
    // known, with why.
    struct WrittenDeclaration {
        std::optional<int64_t> bits;
        std::string text;
        std::string unknown;
    };
    WrittenDeclaration writtenDeclaration(llvm::ArrayRef<cir::CallOp> ctors);
    // A call of rte_mbuf_set_user_mempool_ops or
    // rte_mbuf_set_platform_mempool_ops, which choose the ops a pool made
    // with no ops of its own takes; null if none. Computed once per run.
    Operation *defaultOpsSetter();
    std::optional<Operation *> defaultOpsSet;
    // Whether a single-thread form of a push or pop of this queue type is
    // used anywhere in the module, directly or through its address: then a
    // type whose queues are all unknown may hide a declaration.
    bool hasSingleForm(Type objTy);
    llvm::DenseMap<Type, bool> singleForm;
    // Calls of functions of DPDK's mempool and mbuf libraries that the
    // model does not read, handed a pool or an mbuf: every pool is unknown.
    void poisonUnmodelledMempoolCalls();
    // Pools that may hold mbufs although their creation suggests not: one
    // rte_pktmbuf_pool_init or rte_mempool_obj_iter is handed later.
    void addPoolsInitialisedLater(llvm::ArrayRef<Key> candidates);
    // A declaration that holds only because a side it declares single has
    // no access in this translation unit is reported as vacuously
    // satisfied, not verified: nothing here was checked on that side, and
    // whatever touches it is out of sight. Off, the report reads
    // "verified", as it did before 29 Oct, for comparison with older runs.
    Option<bool> vacuousOutcome{
        *this, "vacuous-outcome",
        llvm::cl::desc("Report a declaration that holds only because a side it declares single has no "
                       "access in this translation unit as \"vacuously satisfied\" rather than "
                       "\"verified\""),
        llvm::cl::init(true)};
    // The rest of the program (see the comment at the top of the file).
    Option<bool> openWorldOption{
        *this, "open-world",
        llvm::cl::desc("Other translation units of the program may name every symbol with external "
                       "linkage here: a unit built to be linked with other files of the program. "
                       "Assumed when the module defines no main and named-elsewhere is not given"),
        llvm::cl::init(false)};
    Option<std::string> namedElsewhereFile{
        *this, "named-elsewhere",
        llvm::cl::desc("A file of the symbols the program's other files define or reference, one per "
                       "line (what nm prints for them): each one this module defines with external "
                       "linkage may be named by another translation unit, and no other may"),
        llvm::cl::init("")};
    bool openWorld = false;
    std::string openWorldWhy;
    llvm::StringSet<> namedElsewhere;
    // Whether another translation unit may name this function or global,
    // and if so why, in the words of a remark.
    bool visibleElsewhere(Operation *symbol, std::string *why = nullptr);
    // Every shim queue type a queue of which leaves this translation unit
    // -- its address handed to a function with no body here, or, where
    // other units may name this one's symbols, kept in or reachable from a
    // global they may name, or handed out by a function they may call --
    // is poisoned: another unit may push to or pop from it on threads the
    // walk never sees.
    void poisonEscapes();
    // Why one side of a queue counts many, in the program's own terms.
    std::string whyMany(const SideInfo &side);
    // The arrays whose elements a constructor call builds one per run of a
    // loop that walks a pointer through them -- the front end's code for
    // `T qs[n];`, `new T[n]` and an array member: a slot holding the first
    // element's address, then that address stepped by a constant, and the
    // call handed what the slot holds. The roots of those arrays, or none.
    llvm::SmallVector<Operation *, 2> arrayCtorRoots(Value object, Instance *inst);
    // A side's count, as SideInfo::count, except that threads started on
    // different arms of one branch -- `if (c) spawn(a); else spawn(b);` in
    // a function that runs once -- are one: at most one of them exists.
    unsigned countOf(const SideInfo &side) const;
    std::string whyThreadMany(const Instance &inst, bool &spawned);
};

// A location as a person reads it: file:line:column, from the first file
// position a fused or named location carries.
static std::string whereOf(Location loc, bool fileOnly = false) {
    if (auto f = dyn_cast<FileLineColLoc>(loc)) {
        StringRef file = f.getFilename().getValue();
        if (fileOnly) file = llvm::sys::path::filename(file);
        return (file + ":" + Twine(f.getLine()) + ":" + Twine(f.getColumn())).str();
    }
    if (auto f = dyn_cast<FusedLoc>(loc))
        for (Location inner : f.getLocations())
            if (std::string w = whereOf(inner, fileOnly); !w.empty()) return w;
    if (auto n = dyn_cast<NameLoc>(loc)) return whereOf(n.getChildLoc(), fileOnly);
    if (auto c = dyn_cast<CallSiteLoc>(loc)) return whereOf(c.getCallee(), fileOnly);
    return "";
}

Access QueueOwnershipAnalysisPass::accessOf(Operation *op, const Instance &inst) {
    // The instance that began this thread: up the chain while the thread
    // stays the same.
    const Instance *r = &inst;
    while (r->parent && r->parent->thread == r->thread) r = r->parent;
    const bool many = inst.threadMult == Mult::Many && !inst.execUnknown;
    cir::FuncOp entry = r->func;
    cir::CallOp site = r->site;
    return Access{op,
                  inst.thread,
                  many,
                  inst.speculative,
                  llvm::demangle(entry.getSymName().str()),
                  site ? site.getLoc() : entry.getLoc(),
                  static_cast<bool>(site),
                  false,
                  &inst};
}

// An mbuf goes back to the pool its `pool` field names, the pool it was
// taken from. That is read here only when the mbuf a free is handed is, in
// the same function, the value an allocation from a pool handed back
// (rte_pktmbuf_alloc, rte_mbuf_raw_alloc, rte_pktmbuf_copy,
// rte_pktmbuf_clone), through a slot stored once. Anything else -- an mbuf
// from a ring, a burst receive, a parameter, an array of them -- names a
// pool the analysis does not follow. A free that walks a chain
// (rte_pktmbuf_free, rte_pktmbuf_free_seg, rte_pktmbuf_free_bulk) puts each
// segment, and the buffer an indirect mbuf is attached to, into its own
// pool, so where the program links mbufs it is not followed either; nor is
// rte_pktmbuf_detach, whose put is into the attached buffer's pool.
Value QueueOwnershipAnalysisPass::mbufPool(Operation *op, std::string *why) {
    auto call = cast<cir::CallOp>(op);
    const LibraryQueueCall l = libraryCall(call);
    const std::string at = whereOf(op->getLoc(), /*fileOnly=*/true);
    auto unresolved = [&](const std::string &w) {
        if (why) *why = w;
        return Value();
    };
    if (l.viaMbuf == MbufFree::Attached)
        return unresolved("an mbuf detached at " + at +
                          " puts the buffer it was attached to into that buffer's pool, which the analysis "
                          "does not name");
    if (l.viaMbuf == MbufFree::Chain)
        if (Operation *link = mbufLinkSite())
            return unresolved("an mbuf freed at " + at + " may have segments or an attached buffer from other "
                              "pools, since mbufs are linked at " + whereOf(link->getLoc(), /*fileOnly=*/true));
    if (auto c = traceToRoot(call.getOperand(l.queueOperand)).getDefiningOp<cir::CallOp>())
        if (LibraryQueueCall a = libraryCall(c); a.returnsMbuf && a.queueOperand < c.getNumOperands())
            return c.getOperand(a.queueOperand);
    return unresolved("an mbuf freed at " + at +
                      " goes back to the pool its `pool` field names, which the analysis does not resolve");
}

Operation *QueueOwnershipAnalysisPass::mbufLinkSite() {
    if (mbufLink) return *mbufLink;
    Operation *site = nullptr;
    module.walk([&](Operation *op) {
        if (auto f = op->getParentOfType<cir::FuncOp>(); f && isLibraryFunction(f)) return WalkResult::advance();
        // rte_pktmbuf_chain, rte_pktmbuf_attach and the like, or the
        // program's own `m->next = other`.
        bool links = false;
        if (auto call = dyn_cast<cir::CallOp>(op)) {
            cir::FuncOp callee = lookupCallee(call);
            links = callee && isMbufLinkFunction(callee.getSymName());
        } else if (auto st = dyn_cast<cir::StoreOp>(op)) {
            auto gm = traceToLocalRoot(st.getAddr()).getDefiningOp<cir::GetMemberOp>();
            auto pt = dyn_cast<cir::PointerType>(st.getValue().getType());
            links = gm && gm.getName() == "next" && pt && isRecordNamed(pt.getPointee(), "rte_mbuf");
        }
        if (!links) return WalkResult::advance();
        site = op;
        return WalkResult::interrupt();
    });
    mbufLink = site;
    return site;
}

Operation *QueueOwnershipAnalysisPass::defaultOpsSetter() {
    if (defaultOpsSet) return *defaultOpsSet;
    Operation *site = nullptr;
    module.walk([&](cir::CallOp call) {
        if (auto f = call->getParentOfType<cir::FuncOp>(); f && isLibraryFunction(f)) return WalkResult::advance();
        cir::FuncOp callee = lookupCallee(call);
        if (!callee || (callee.getSymName() != "rte_mbuf_set_user_mempool_ops" &&
                        callee.getSymName() != "rte_mbuf_set_platform_mempool_ops"))
            return WalkResult::advance();
        site = call;
        return WalkResult::interrupt();
    });
    defaultOpsSet = site;
    return site;
}

bool QueueOwnershipAnalysisPass::hasSingleForm(Type objTy) {
    auto [cached, fresh] = singleForm.try_emplace(objTy, false);
    if (!fresh) return cached->second;
    bool found = false;
    module.walk([&](cir::FuncOp f) {
        LibraryQueueFunction l = libraryFunction(f);
        if (l.sync != LibraryQueueSync::Single) return WalkResult::advance();
        const unsigned arg = (firstArgIsSret(f) ? 1 : 0) + l.queueArg;
        auto inputs = f.getFunctionType().getInputs();
        auto pt = arg < inputs.size() ? dyn_cast<cir::PointerType>(inputs[arg]) : cir::PointerType();
        if (!pt || pt.getPointee() == objTy) {
            found = true;
            return WalkResult::interrupt();
        }
        return WalkResult::advance();
    });
    singleForm[objTy] = found;
    return found;
}

void QueueOwnershipAnalysisPass::poisonUnmodelledMempoolCalls() {
    module.walk([&](cir::CallOp call) {
        if (auto f = call->getParentOfType<cir::FuncOp>(); f && isLibraryFunction(f)) return;
        cir::FuncOp callee = lookupCallee(call);
        if (!callee || !libraryFunction(callee).unmodelled) return;
        for (Value v : call.getArgOperands()) {
            auto pt = dyn_cast<cir::PointerType>(v.getType());
            Type poolTy = pt && isRecordNamed(pt.getPointee(), "rte_mempool") ? pt.getPointee()
                                                                               : mbufPoolType(v.getType());
            if (!poolTy) continue;
            const std::string why =
                (callee.getSymName() + " is handed " + (pt && pt.getPointee() == poolTy ? "a pool" : "an mbuf") +
                 ", and the model does not read what it puts or gets")
                    .str();
            if (poisonedTypes.insert(poolTy).second) poisonWhy[poolTy] = why;
            call.emitRemark() << "wtc: " << why << "; every queue of its type is now unknown";
            ++remarks;
            return;
        }
    });
}

void QueueOwnershipAnalysisPass::addPoolsInitialisedLater(llvm::ArrayRef<Key> candidates) {
    if (candidates.empty()) return;
    bool all = false;
    llvm::SmallSetVector<Key, 4> found;
    module.walk([&](cir::CallOp call) {
        if (all) return WalkResult::interrupt();
        if (auto f = call->getParentOfType<cir::FuncOp>(); f && isLibraryFunction(f)) return WalkResult::advance();
        cir::FuncOp callee = lookupCallee(call);
        if (!callee || (callee.getSymName() != "rte_pktmbuf_pool_init" &&
                        callee.getSymName() != "rte_mempool_obj_iter"))
            return WalkResult::advance();
        // rte_mempool_obj_iter(mp, cb, arg) with a null callback sets
        // nothing up; any other may set up mbufs.
        const unsigned k = firstRealOperand(call);
        if (callee.getSymName() == "rte_mempool_obj_iter" && k + 1 < call.getNumOperands() &&
            isNullPointer(call.getOperand(k + 1)))
            return WalkResult::advance();
        auto in = call->getParentOfType<cir::FuncOp>();
        llvm::ArrayRef<Instance *> insts = in ? walk->instancesOf(in) : llvm::ArrayRef<Instance *>();
        if (k >= call.getNumOperands() || insts.empty()) { all = true; return WalkResult::interrupt(); }
        for (Instance *inst : insts)
            for (const Place &pl : walk->resolvePlaces(call.getOperand(k), inst)) {
                if (!pl.root) { all = true; return WalkResult::interrupt(); }
                found.insert({pl.root, pl.offset});
            }
        return WalkResult::advance();
    });
    for (const Key &c : candidates)
        if (all || found.contains(c)) mbufPoolKeys.insert(c);
}

// Why one thread the walk saw stands for many: the nearest cause above the
// instance that starts it -- a spawn or a call in a loop, recursion, or a
// root nothing here accounts for. Empty when the cause is a guess (a goto
// with no loop), which the count keeps but a programmer could not act on.
// `spawned` is false when the thread is such a root itself: a function
// another file may call directly, a weaker account than the spawn that
// starts it here, when there is one.
std::string QueueOwnershipAnalysisPass::whyThreadMany(const Instance &inst, bool &spawned) {
    const Instance *start = &inst;
    while (start->parent && start->parent->thread == start->thread) start = start->parent;
    spawned = true;
    for (const Instance *q = start; q; q = q->parent) {
        cir::FuncOp f = q->func;   // op handles are not const
        cir::CallOp site = q->site;
        const std::string fn = llvm::demangle(f.getSymName().str());
        if (q->recurring) return fn + " is reached again through recursion";
        if (!site) {
            if (!q->manyRoot) return "";
            spawned = q != start;
            return spawned ? "started from " + fn + ", which may run any number of times: " + q->manyRoot
                           : fn + " may run any number of times: " + q->manyRoot;
        }
        bool known;
        const Mult m = walk->siteMult(site.getOperation(), q->parent->func, known);
        if (!known) return "";
        if (m == Mult::Many) {
            if (cir::FuncOp c = lookupCallee(site); c && c.getSymName() == "rte_eal_mp_remote_launch")
                return "started on every worker lcore at " + whereOf(site.getLoc(), /*fileOnly=*/true);
            const bool spawn = getSemanticKind(lookupCallee(site)) == SemanticOpKind::ThreadSpawn;
            return std::string(spawn ? "started" : "reached by a call") + " in a loop at " +
                   whereOf(site.getLoc(), /*fileOnly=*/true);
        }
    }
    return "";
}

llvm::SmallVector<Operation *, 2> QueueOwnershipAnalysisPass::arrayCtorRoots(Value object, Instance *inst) {
    llvm::SmallVector<Operation *, 2> out;
    auto load = object.getDefiningOp<cir::LoadOp>();
    auto slot = load ? load.getAddr().getDefiningOp<cir::AllocaOp>() : cir::AllocaOp();
    if (!slot) return out;
    // The call runs in a loop, and the loop steps the slot: without both,
    // the slot is a parameter or a pointer set once, and the call builds
    // one object, as any other.
    auto loop = load->getParentOfType<cir::LoopOpInterface>();
    if (!loop) return out;
    Value first;
    bool stepped = false;
    for (Operation *u : slot.getResult().getUsers()) {
        if (isa<cir::LoadOp>(u)) continue;
        auto st = dyn_cast<cir::StoreOp>(u);
        if (!st || st.getAddr() != slot.getResult()) return {};   // the address escapes
        const bool inLoop = loop.getOperation()->isProperAncestor(st);
        // A step: the slot's own value moved by a constant. One in the loop
        // is needed; others walk the same array (the cleanup's loop back).
        if (auto ps = st.getValue().getDefiningOp<cir::PtrStrideOp>()) {
            auto from = ps.getBase().getDefiningOp<cir::LoadOp>();
            auto by = traceToLocalRoot(ps.getStride()).getDefiningOp<cir::ConstantOp>();
            if (from && from.getAddr() == slot.getResult() && by) { stepped |= inLoop; continue; }
        }
        // The start, before the loop.
        if (first || inLoop) return {};
        first = st.getValue();
    }
    if (!first || !stepped) return out;
    // The start is the first element of an array the walk can place: a
    // global or a slot of an array of the type, or a heap block.
    for (const Place &pl : walk->resolvePlaces(first, inst))
        if (pl.root && !llvm::is_contained(out, pl.root)) out.push_back(pl.root);
        else if (!pl.root) return {};
    return out;
}

unsigned QueueOwnershipAnalysisPass::countOf(const SideInfo &side) const {
    const unsigned n = side.count();
    if (n != 2 || side.anyMany || side.threads.size() < 2) return n;
    for (size_t i = 0; i < side.threads.size(); ++i)
        for (size_t j = i + 1; j < side.threads.size(); ++j)
            if (!walk->exclusive(side.threads[i], side.threads[j])) return n;
    return 1;
}

// Two threads that each touch the side, or one that stands for many and
// why. The first explained access answers: one cause is what a programmer
// needs to look at, and every access of one thread shares it.
std::string QueueOwnershipAnalysisPass::whyMany(const SideInfo &side) {
    std::string weak;
    for (const Access &a : side.accesses) {
        if (!a.many || !a.inst) continue;
        bool spawned;
        std::string w = whyThreadMany(*a.inst, spawned);
        if (w.empty()) continue;
        if (spawned) return w;
        if (weak.empty()) weak = std::move(w);
    }
    if (!weak.empty()) return weak;
    if (side.threads.size() >= 2) {
        llvm::SmallVector<std::string, 4> named;
        llvm::SmallSet<unsigned, 4> seen;
        for (const Access &a : side.accesses) {
            if (!seen.insert(a.thread).second) continue;
            std::string w = whereOf(a.started, /*fileOnly=*/true);
            named.push_back(a.entry + (w.empty() ? "" : (a.spawned ? " started at " : " at ") + w));
        }
        std::string out = std::to_string(side.threads.size()) + " threads: ";
        for (size_t i = 0; i < named.size() && i < 3; ++i) out += (i ? ", " : "") + named[i];
        if (named.size() > 3) out += ", ...";
        return out;
    }
    return "";
}

const Instance *const QueueOwnershipAnalysisPass::kAmbiguous = reinterpret_cast<const Instance *>(1);

// A parameter names what the instance's call site passed, as a place is
// resolved: once per call string, so a helper that takes the queue by
// reference contributes, under each of its instances, the member that
// instance's site handed it. Null when a parameter has no site to read.
static Value throughParameters(Value t, Instance *&inst) {
    for (unsigned hops = 0; hops < 64; ++hops) {
        auto arg = dyn_cast<BlockArgument>(t);
        if (!arg) return t;
        if (!inst || !inst->site || arg.getOwner() != &inst->func.getBody().front()) return Value();
        cir::CallOp site = inst->site;
        unsigned idx = arg.getArgNumber();
        if (getSemanticKind(lookupCallee(site)) == SemanticOpKind::ThreadSpawn) idx += spawnFirstArgOperand(site);
        if (idx >= site.getNumOperands()) return Value();
        t = traceToRoot(site.getOperand(idx));
        inst = inst->parent;
    }
    return Value();
}

std::optional<AbstractQueue> QueueOwnershipAnalysisPass::memberOf(Value handle, Instance *inst, Value *base,
                                                                  Instance **baseInst) {
    Instance *ctx = inst;
    Value t = throughParameters(traceToRoot(handle), ctx);
    auto gm = t ? t.getDefiningOp<cir::GetMemberOp>() : cir::GetMemberOp();
    if (!gm) return std::nullopt;
    if (!layout) layout.emplace(module);
    AbstractQueue a{Type(), 0, gm.getName().str()};
    while (gm) {
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
        if (!rec || rec.isIncomplete()) return std::nullopt;
        a.offset += rec.getElementOffset(*layout, gm.getIndex());
        a.record = rec;
        t = traceToRoot(gm.getAddr());
        gm = t.getDefiningOp<cir::GetMemberOp>();
    }
    if (base) *base = t;
    if (baseInst) *baseInst = ctx;
    return a;
}

bool QueueOwnershipAnalysisPass::sameObject(Operation *op, Instance *inst, const Instance *holder) {
    if (!holder || holder == kAmbiguous) return false;
    Value base;
    Instance *at = nullptr;
    if (!memberOf(queueOf(op), inst, &base, &at)) return false;
    // The base is `this` of some method, handed down the chain from the
    // closure that captured it.
    base = throughParameters(base, at);
    auto ld = base ? base.getDefiningOp<cir::LoadOp>() : cir::LoadOp();
    if (!ld) return false;
    auto gm = traceToLocalRoot(ld.getAddr()).getDefiningOp<cir::GetMemberOp>();
    if (!gm) return false;
    auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
    auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
    if (!rec || rec.isIncomplete()) return false;
    const std::pair<Type, unsigned> field{rec, gm.getIndex()};
    auto it = captureStores.find(field);
    if (it == captureStores.end()) {
        llvm::SmallVector<cir::StoreOp, 2> stores;
        bool listed = true;
        ModuleOp m = module;
        m.walk([&](cir::GetMemberOp g) {
            if (!listed) return;
            auto p = dyn_cast<cir::PointerType>(g.getAddr().getType());
            if (!p || p.getPointee() != rec || g.getIndex() != gm.getIndex()) return;
            for (Operation *u : g->getUsers()) {
                if (isa<cir::LoadOp>(u)) continue;
                if (auto st = dyn_cast<cir::StoreOp>(u); st && st.getAddr() == g.getResult()) {
                    stores.push_back(st);
                    continue;
                }
                listed = false;   // the capture's address goes elsewhere
            }
        });
        if (!listed) stores.clear();
        it = captureStores.try_emplace(field, std::move(stores)).first;
    }
    if (it->second.empty()) return false;
    cir::FuncOp holderFn = holder->func;
    for (cir::StoreOp st : it->second) {
        Value v = traceToRoot(st.getValue());
        // A closure that owns something is moved into the thread's state
        // field by field: the capture is stored from the same capture of
        // another closure, which holds a value already listed here.
        if (auto ld = v.getDefiningOp<cir::LoadOp>()) {
            auto from = traceToLocalRoot(ld.getAddr()).getDefiningOp<cir::GetMemberOp>();
            auto fp = from ? dyn_cast<cir::PointerType>(from.getAddr().getType()) : cir::PointerType();
            if (fp && fp.getPointee() == rec && from.getIndex() == gm.getIndex()) continue;
            return false;
        }
        auto arg = dyn_cast<BlockArgument>(v);
        if (!arg || arg.getArgNumber() != 0 || arg.getOwner()->getParentOp() != holderFn.getOperation())
            return false;
    }
    return true;
}

std::optional<Key> QueueOwnershipAnalysisPass::abstractKey(Operation *op, Instance *inst) {
    std::optional<AbstractQueue> m = memberOf(queueOf(op), inst);
    if (!m) return std::nullopt;
    auto it = memberCtors.find({m->record, m->offset});
    if (it == memberCtors.end() || it->second.size() != 1) return std::nullopt;
    Operation *ctor = it->second.front();
    auto built = dyn_cast<cir::PointerType>(cast<cir::CallOp>(ctor).getOperand(0).getType());
    if (!built || built.getPointee() != queueObjectTypeOfOp(op)) return std::nullopt;
    const Key k{ctor, kAbstractOffset};
    if (abstractKeys.try_emplace(k, *m).second) {
        keyType[k] = built.getPointee();
        keyCtors[k].insert(ctor);
        ctorKeys[ctor].insert(k);
    }
    return k;
}

Operation *QueueOwnershipAnalysisPass::carrierOf(const Key &k, const char *&why) const {
    // An abstract queue is the one its constructor call builds, by
    // construction: every object it builds is among those it stands for.
    if (abstractKeys.count(k)) return k.first;
    auto it = keyCtors.find(k);
    if (it == keyCtors.end() || it->second.empty()) { why = "no constructor call is seen to build it"; return nullptr; }
    if (it->second.size() != 1) { why = "more than one constructor call may build it"; return nullptr; }
    Operation *c = it->second.front();
    if (ctorUnplaced.contains(c)) { why = "its constructor call also builds an object the analysis cannot name"; return nullptr; }
    if (ctorKeys.lookup(c).size() != 1) { why = "its constructor call also builds other queues"; return nullptr; }
    return c;
}

llvm::StringRef QueueOwnershipAnalysisPass::pointerNaming(Operation *allocation) const {
    if (!pointerNames) {
        pointerNames.emplace();
        llvm::DenseMap<llvm::StringRef, llvm::SmallVector<cir::StoreOp, 1>> storesOf;
        llvm::DenseSet<llvm::StringRef> escaped;
        ModuleOp m = module;
        m.walk([&](cir::GetGlobalOp g) {
            for (Operation *u : g.getResult().getUsers()) {
                if (auto st = dyn_cast<cir::StoreOp>(u); st && st.getAddr() == g.getResult())
                    storesOf[g.getName()].push_back(st);
                else if (!isa<cir::LoadOp>(u))
                    escaped.insert(g.getName());
            }
        });
        for (auto &[name, stores] : storesOf) {
            if (stores.size() != 1 || escaped.contains(name)) continue;
            if (Operation *src = traceToRoot(stores.front().getValue()).getDefiningOp())
                if (isa<cir::CallOp>(src)) (*pointerNames)[src] = name;
        }
    }
    return pointerNames->lookup(allocation);
}

std::string QueueOwnershipAnalysisPass::labelOf(const Key &k, bool whole) const {
    Operation *root = k.first;
    if (auto it = abstractKeys.find(k); it != abstractKeys.end()) {
        auto rec = cast<cir::RecordType>(it->second.record);
        const std::string name = rec.getName() ? rec.getName().getValue().str() : "an unnamed record";
        return it->second.member + " in any " + name;
    }
    std::string base;
    if (auto g = dyn_cast<cir::GlobalOp>(root)) base = ("@" + g.getSymName()).str();
    else if (auto a = dyn_cast<cir::AllocaOp>(root)) base = ("%" + a.getName()).str();
    else if (llvm::StringRef name = pointerNaming(root); !name.empty()) base = ("*@" + name).str();
    else {
        // ClangIR gives an allocation the location of its function; the
        // constructor's is the new expression's.
        Location at = root->getLoc();
        if (auto it = keyCtors.find(k); it != keyCtors.end() && !it->second.empty())
            at = it->second.front()->getLoc();
        base = "the heap object made at " + whereOf(at, /*fileOnly=*/true);
    }
    if (whole) return base;
    // Every element of an array, written as the array indexed by any index.
    if (k.second == kElementsOffset) return base + "[*]";
    // The member's name, from the address its constructor is handed.
    std::string member;
    if (auto it = keyCtors.find(k); it != keyCtors.end())
        for (Operation *c : it->second)
            if (auto gm = cast<cir::CallOp>(c).getOperand(0).getDefiningOp<cir::GetMemberOp>())
                member = gm.getName().str();
    if (isa<cir::CallOp>(root))
        return member.empty() ? (k.second ? base + ", at offset " + std::to_string(k.second) : base)
                              : member + " in " + base;
    return member.empty() ? base + "+" + std::to_string(k.second) : base + "." + member;
}

// An integer the program fixes before it runs: a constant, an or of such,
// through integral casts and a slot stored once. DPDK's flags are spelled
// `RING_F_SP_ENQ | RING_F_SC_DEQ`, and often kept in a local first.
static std::optional<int64_t> constantInteger(Value v, unsigned depth = 0) {
    if (!v || depth > 8) return std::nullopt;
    v = traceToRoot(v);
    if (auto c = v.getDefiningOp<cir::ConstantOp>())
        if (auto i = dyn_cast<cir::IntAttr>(c.getValue())) return i.getSInt();
    if (auto c = v.getDefiningOp<cir::CastOp>()) return constantInteger(c.getSrc(), depth + 1);
    if (auto o = v.getDefiningOp<cir::OrOp>()) {
        auto l = constantInteger(o.getLhs(), depth + 1), r = constantInteger(o.getRhs(), depth + 1);
        if (l && r) return *l | *r;
    }
    return std::nullopt;
}

// A null pointer the program writes: `NULL`, `0`.
static bool isNullPointer(Value v) {
    if (!v) return false;
    v = traceToRoot(v);
    if (auto c = v.getDefiningOp<cir::ConstantOp>()) {
        if (auto p = dyn_cast<cir::ConstPtrAttr>(c.getValue())) return p.isNullValue();
        if (auto i = dyn_cast<cir::IntAttr>(c.getValue())) return i.isNullValue();
    }
    if (auto c = v.getDefiningOp<cir::CastOp>()) return isNullPointer(c.getSrc());
    return false;
}

// A string the program fixes before it runs: a literal, through the
// array's decay and a slot stored once.
static std::optional<std::string> constantString(Value v) {
    if (!v) return std::nullopt;
    v = traceToRoot(v);
    auto gg = v.getDefiningOp<cir::GetGlobalOp>();
    auto g = gg ? SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr()) : cir::GlobalOp();
    if (!g || !g.getConstant() || !g.getInitialValue()) return std::nullopt;
    auto arr = dyn_cast<cir::ConstArrayAttr>(*g.getInitialValue());
    auto str = arr ? dyn_cast<StringAttr>(arr.getElts()) : StringAttr();
    if (!str) return std::nullopt;
    return str.getValue().split('\0').first.str();
}

// The sides a mempool's ops name single (1: one producer, 2: one
// consumer): DPDK's ring driver registers the four ring_{sp,mp}_{sc,mc};
// every other ops it ships -- ring_mt_rts, ring_mt_hts, stack, lf_stack,
// bucket, the hardware pools -- takes any number on both sides.
static int64_t opsBits(llvm::StringRef ops) {
    if (ops == "ring_sp_sc") return 3;
    if (ops == "ring_sp_mc") return 1;
    if (ops == "ring_mp_sc") return 2;
    return 0;
}

// Whether a pool may hold mbufs, so that a free whose pool is not named
// may put into it: one made by rte_pktmbuf_pool_create and its forms, or
// by rte_mempool_create_empty, whose objects the program sets up later;
// one made by rte_mempool_create only when it is handed an initialiser
// (rte_pktmbuf_pool_init, rte_pktmbuf_init, or any of the program's,
// which may call them). A pool made with neither holds what the program
// puts there.
static bool mayHoldMbufs(cir::CallOp create) {
    cir::FuncOp f = lookupCallee(create);
    if (!f || f.getSymName() != "rte_mempool_create") return true;
    // rte_mempool_create(name, n, elt_size, cache_size, private_data_size,
    //   mp_init, mp_init_arg, obj_init, obj_init_arg, socket_id, flags)
    const unsigned k = firstRealOperand(create);
    for (unsigned i : {5u, 7u})
        if (k + i >= create.getNumOperands() || !isNullPointer(create.getOperand(k + i))) return true;
    return false;
}

// Whether a function's first argument is the hidden return slot.
static bool firstArgIsSret(cir::FuncOp f) {
    auto attrs = f.getArgAttrsAttr();
    if (!attrs || attrs.empty()) return false;
    auto d = dyn_cast<DictionaryAttr>(attrs[0]);
    return d && d.get("llvm.sret");
}

// What a modelled library queue's creation declares, as written. An
// rte_ring's flags (RING_F_SP_ENQ 0x1, RING_F_SC_DEQ 0x2, rte_ring_core.h)
// at rte_ring_create, rte_ring_create_elem or rte_ring_init. A mempool's
// ops: named by rte_mempool_set_ops_byname, which replaces whatever its
// creation chose, or by rte_pktmbuf_pool_create_by_ops; else picked from
// its flags (RTE_MEMPOOL_F_SP_PUT 0x4, RTE_MEMPOOL_F_SC_GET 0x8,
// rte_mempool.h; MEMPOOL_F_SP_PUT and MEMPOOL_F_SC_GET before DPDK 21.11,
// the same values) by rte_mempool_create and rte_mempool_create_empty; and
// for rte_pktmbuf_pool_create, or a null ops name, the default ops, which
// the program does not choose and which declare nothing single. Flags or
// ops computed at run time, given differently by two creations or two
// settings, or never seen because the creation is out of sight are not
// known.
QueueOwnershipAnalysisPass::WrittenDeclaration
QueueOwnershipAnalysisPass::writtenDeclaration(llvm::ArrayRef<cir::CallOp> ctors) {
    WrittenDeclaration w;
    const bool pool = !ctors.empty() && libraryCall(ctors.front()).kind == LibraryQueueKind::Mempool;
    llvm::SmallVector<cir::CallOp, 2> deciding;
    if (pool)
        for (cir::CallOp c : ctors)
            if (LibraryQueueCall l = libraryCall(c); l.role == LibraryQueueRole::Construct && l.opsOperand)
                deciding.push_back(c);
    const bool setByName = !deciding.empty();
    // Only a creation, or a call that sets flags: populating a pool built
    // elsewhere declares nothing.
    if (!setByName)
        for (cir::CallOp c : ctors)
            if (LibraryQueueCall l = libraryCall(c);
                l.role == LibraryQueueRole::Create || l.flagsOperand || l.opsOperand)
                deciding.push_back(c);
    if (deciding.empty()) {
        w.unknown = "its creation is not in this translation unit, so its flags are not known";
        return w;
    }
    for (cir::CallOp c : deciding) {
        LibraryQueueCall l = libraryCall(c);
        auto operand = [&](std::optional<unsigned> i) {
            return i && *i < c.getNumOperands() ? c.getOperand(*i) : Value();
        };
        int64_t bits = 0;
        std::string text;
        // The default ops, for a pool whose creation names none: the
        // program may have set them.
        const bool defaultOps = pool && l.role == LibraryQueueRole::Create && !l.flagsOperand &&
                                (!l.opsOperand || isNullPointer(operand(l.opsOperand)));
        if (defaultOps)
            if (Operation *set = defaultOpsSetter()) {
                w.unknown = ("its ops are the default ops, which " +
                             lookupCallee(cast<cir::CallOp>(set)).getSymName() + " sets at " +
                             whereOf(set->getLoc(), /*fileOnly=*/true))
                                .str();
                return w;
            }
        if (l.opsOperand) {
            Value v = operand(l.opsOperand);
            if (!isNullPointer(v)) {
                std::optional<std::string> ops = constantString(v);
                if (!ops) {
                    w.unknown = "its ops are chosen at run time";
                    return w;
                }
                bits = opsBits(*ops);
                text = "by its ops \"" + *ops + "\"";
            }
        } else if (l.flagsOperand) {
            std::optional<int64_t> f = constantInteger(operand(l.flagsOperand));
            if (!f) {
                w.unknown = "its flags are computed at run time";
                return w;
            }
            bits = pool ? ((*f & 4) ? 1 : 0) | ((*f & 8) ? 2 : 0) : (*f & 3);
            const char *sp = pool ? "RTE_MEMPOOL_F_SP_PUT" : "RING_F_SP_ENQ";
            const char *sc = pool ? "RTE_MEMPOOL_F_SC_GET" : "RING_F_SC_DEQ";
            text = "by its flags ";
            if (bits & 1) text += sp;
            if (bits == 3) text += " | ";
            if (bits & 2) text += sc;
        }
        if (w.bits && *w.bits != bits) {
            w.unknown = setByName ? "it is given different ops" : "its creations give it different flags";
            return w;
        }
        w.bits = bits;
        if (w.text.empty()) w.text = text;
    }
    return w;
}

// What a modelled library queue declares. A class declares one thread on
// each side through its type, whether its constructor is in sight or only
// its pushes and pops are. An rte_ring declares a side single through an
// rte_ring_sp_* or rte_ring_sc_* call on that side, and an rte_ring or a
// mempool through what its creation writes (writtenDeclaration) when some
// call on that side follows it or when no call on that side is seen at
// all: a side reached only by rte_ring_mp_* or rte_ring_mc_* calls is many
// whatever the flags say. The declaration is reported as written, with
// what qualifies it on each side. One that is not known leaves such a side
// unknown.
QueueOwnershipAnalysisPass::LibraryDeclaration
QueueOwnershipAnalysisPass::libraryDeclared(llvm::ArrayRef<cir::CallOp> ctors, const RootInfo &info) {
    LibraryDeclaration d;
    auto syncOf = [this](Operation *op) { return libraryOp(op).sync; };
    auto byTypeCall = [&](Operation *op) {
        auto call = dyn_cast<cir::CallOp>(op);
        return call && libraryCall(call).sync == LibraryQueueSync::ByType;
    };
    auto byTypeAccess = [&](const Access &a) { return byTypeCall(a.op); };
    const bool byType = llvm::any_of(ctors, [&](cir::CallOp c) { return byTypeCall(c.getOperation()); }) ||
                        llvm::any_of(info.producers.accesses, byTypeAccess) ||
                        llvm::any_of(info.consumers.accesses, byTypeAccess);
    if (byType) {
        d.producers = d.consumers = Declared::One;
        d.how = "by its type";
        return d;
    }
    const WrittenDeclaration w = writtenDeclaration(ctors);
    d.unknown = w.unknown;
    // The declaration as written, then what qualifies it on a side, then
    // the calls that declare a side single themselves.
    const bool known = d.unknown.empty() && w.bits;
    std::string byFlags, byCalls;
    if (known && (*w.bits & 3)) byFlags = w.text;
    for (auto [side, out, bit, name] : {std::tuple{&info.producers, &d.producers, int64_t(1), "producer"},
                                        std::tuple{&info.consumers, &d.consumers, int64_t(2), "consumer"}}) {
        const Access *single = nullptr;
        bool followsFlags = false;
        for (const Access &a : side->accesses) {
            const LibraryQueueSync sync = syncOf(a.op);
            if (sync == LibraryQueueSync::Single && !single) single = &a;
            if (sync == LibraryQueueSync::ByFlags) followsFlags = true;
        }
        const bool flagged = known && (*w.bits & bit);
        if (single) {
            *out = Declared::One;
            auto call = cast<cir::CallOp>(single->op);
            if (!byCalls.empty()) byCalls += ", and ";
            byCalls += ("by " + lookupCallee(call).getSymName() + " at " +
                        whereOf(call.getLoc(), /*fileOnly=*/true)).str();
        } else if (followsFlags || side->accesses.empty()) {
            if (!d.unknown.empty()) *out = Declared::Unknown;
            else if (flagged) {
                *out = Declared::One;
                if (side->accesses.empty()) byFlags += std::string("; no ") + name + " access seen";
            }
        } else if (flagged) {
            byFlags += std::string("; its ") + name + "s call the explicit multi-thread form, which the flag does not bind";
        }
    }
    d.how = byFlags;
    if (!byCalls.empty()) d.how += d.how.empty() ? byCalls : ", and " + byCalls;
    return d;
}

std::string QueueOwnershipAnalysisPass::openWhy(Type t) {
    auto [it, fresh] = openWhyCache.try_emplace(t);
    if (!fresh) return it->second;
    std::string why;
    const bool ring = isRecordNamed(t, "rte_ring"), pool = isRecordNamed(t, "rte_mempool");
    // A pointer to a queue of this type, or to one (an array of rings).
    auto pointsToQueue = [&](Type ty) {
        auto p = dyn_cast<cir::PointerType>(ty);
        if (!p) return false;
        if (p.getPointee() == t) return true;
        auto pp = dyn_cast<cir::PointerType>(p.getPointee());
        return pp && pp.getPointee() == t;
    };
    // A record with such a pointer among its members, directly, in an
    // array member, or in a record held by value at any depth: a structure
    // of parameters naming a ring, or DPDK's per-lcore configuration with
    // its array of rings. Records cannot hold themselves by value, so the
    // descent ends; `seen` only spares repeated work.
    llvm::DenseSet<Type> seen;
    std::function<bool(Type)> holdsQueue = [&](Type ty) {
        while (auto arr = dyn_cast_or_null<cir::ArrayType>(ty)) ty = arr.getElementType();
        if (pointsToQueue(ty)) return true;
        auto rec = dyn_cast_or_null<cir::RecordType>(ty);
        if (!rec || rec.isIncomplete() || !seen.insert(rec).second) return false;
        return llvm::any_of(rec.getMembers(), [&](Type m) { return holdsQueue(m); });
    };
    // What an operand hands over, seen through the casts to void * it
    // went through: (void *)ring is a ring, (void *)&conf a structure.
    auto reaches = [&](Value v) {
        for (Type ty : {v.getType(), traceToLocalRoot(v).getType()}) {
            auto p = dyn_cast<cir::PointerType>(ty);
            if (pointsToQueue(ty) || (p && holdsQueue(p.getPointee()))) return true;
        }
        return false;
    };
    // Whether a pointer to a queue of this type is stored into a member of
    // some record, or into an element of an array member: then a void *
    // handed to code out of sight may hold it.
    bool storedInRecord = false;
    module.walk([&](cir::StoreOp st) {
        if (!pointsToQueue(st.getValue().getType())) return WalkResult::advance();
        Value a = traceToLocalRoot(st.getAddr());
        for (unsigned depth = 0; depth < 8; ++depth) {
            if (auto ps = a.getDefiningOp<cir::PtrStrideOp>()) a = traceToLocalRoot(ps.getBase());
            else if (auto ge = a.getDefiningOp<cir::GetElementOp>()) a = traceToLocalRoot(ge.getBase());
            else break;
        }
        if (a.getDefiningOp<cir::GetMemberOp>()) {
            storedInRecord = true;
            return WalkResult::interrupt();
        }
        return WalkResult::advance();
    });
    auto isVoidPointer = [](Type ty) {
        auto p = dyn_cast<cir::PointerType>(ty);
        return p && isa<cir::VoidType>(p.getPointee());
    };
    // A pool's mbufs handed to code out of sight -- a driver's burst
    // function, called through a pointer by rte_eth_tx_burst -- may be
    // freed or refilled there, on the thread that hands them over.
    auto handsMbufs = [&](cir::CallOp call) {
        return pool && llvm::any_of(call.getArgOperands(), [](Value v) { return isMbufPointer(v.getType()); });
    };
    module.walk([&](cir::CallOp call) {
        if (!why.empty()) return WalkResult::interrupt();
        if (auto f = call->getParentOfType<cir::FuncOp>(); f && isLibraryFunction(f))
            return WalkResult::advance();
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) {
            if (call.isIndirect() && handsMbufs(call)) {
                auto in = call->getParentOfType<cir::FuncOp>();
                why = "mbufs are handed to a call through a pointer in " +
                      (in ? llvm::demangle(in.getSymName().str()) : std::string("the program")) +
                      ", which may free them or take more from a pool";
            }
            return WalkResult::advance();
        }
        if (ring && callee.getSymName() == "rte_ring_lookup") {
            why = "rte_ring_lookup hands rings to code that finds them by name";
            return WalkResult::interrupt();
        }
        if (pool && callee.getSymName() == "rte_mempool_lookup") {
            why = "rte_mempool_lookup hands pools to code that finds them by name";
            return WalkResult::interrupt();
        }
        // A thread start's argument goes to an entry the walk follows.
        if (!callee.getBody().empty() || isLibraryFunction(callee) || isAllocator(callee) ||
            touchesOnlyItsArguments(callee) || isCQueueRuntime(callee) || callee.getSymName() == "free" ||
            getSemanticKind(callee) == SemanticOpKind::ThreadSpawn)
            return WalkResult::advance();
        if (llvm::any_of(call.getOperands(), reaches))
            why = ("it is handed to " + callee.getSymName() + ", whose body is not here").str();
        else if (handsMbufs(call))
            why = ("mbufs are handed to " + callee.getSymName() +
                   ", whose body is not here and may free them or take more from a pool")
                      .str();
        else if (storedInRecord &&
                 llvm::any_of(call.getOperands(), [&](Value v) { return isVoidPointer(v.getType()); }))
            why = ("it is kept in a structure, and " + callee.getSymName() +
                   ", whose body is not here, takes a void * that may hold one")
                      .str();
        return WalkResult::advance();
    });
    it = openWhyCache.find(t);
    it->second = why;
    return why;
}

void QueueOwnershipAnalysisPass::poisonAddressTakenLibraryOps() {
    // Every use of the function's symbol that is not a direct call takes
    // its address: a cir.get_global, a global's initialiser
    // (#cir.global_view), a member-function pointer (#cir.method, or the
    // __const. global holding one), a thread's entry. Uses inside the
    // library's own functions are the library's.
    module.walk([&](cir::FuncOp f) {
        LibraryQueueFunction l = libraryFunction(f);
        if (l.role != LibraryQueueRole::Push && l.role != LibraryQueueRole::Pop) return;
        std::optional<SymbolTable::UseRange> uses = SymbolTable::getSymbolUses(f, module);
        Operation *taken = nullptr;
        if (!uses) taken = f.getOperation();
        else
            for (const SymbolTable::SymbolUse &u : *uses) {
                Operation *user = u.getUser();
                if (auto call = dyn_cast<cir::CallOp>(user); call && !call.isIndirect()) continue;
                if (auto in = user->getParentOfType<cir::FuncOp>(); in && isLibraryFunction(in)) continue;
                taken = user;
                break;
            }
        if (!taken) return;
        const unsigned arg = (firstArgIsSret(f) ? 1 : 0) + l.queueArg;
        auto inputs = f.getFunctionType().getInputs();
        auto pt = arg < inputs.size() ? dyn_cast<cir::PointerType>(inputs[arg]) : cir::PointerType();
        // A free's queue is the pool its mbuf names.
        Type queueTy = !pt ? Type() : l.viaMbuf != MbufFree::None ? mbufPoolType(pt) : pt.getPointee();
        const std::string why = "the address of " + llvm::demangle(f.getSymName().str()) +
                                " is taken, and calls through it are not counted";
        if (!queueTy) globalPoison = true;
        else if (poisonedTypes.insert(queueTy).second) poisonWhy[queueTy] = why;
        taken->emitRemark() << "wtc: " << why << "; every queue of its type is now unknown";
        ++remarks;
    });
}

bool QueueOwnershipAnalysisPass::visibleElsewhere(Operation *symbol, std::string *why) {
    std::optional<cir::GlobalLinkageKind> linkage;
    llvm::StringRef name;
    if (auto f = dyn_cast<cir::FuncOp>(symbol)) { linkage = f.getLinkage(); name = f.getSymName(); }
    else if (auto g = dyn_cast<cir::GlobalOp>(symbol)) { linkage = g.getLinkage(); name = g.getSymName(); }
    if (!linkage || *linkage == cir::GlobalLinkageKind::InternalLinkage ||
        *linkage == cir::GlobalLinkageKind::PrivateLinkage)
        return false;
    // A global only declared here is defined in another unit, which
    // names it by definition.
    if (auto g = dyn_cast<cir::GlobalOp>(symbol); g && g.isDeclaration()) {
        if (why) *why = "it is defined in another translation unit";
        return true;
    }
    if (openWorld) {
        if (why) *why = openWorldWhy;
        return true;
    }
    if (namedElsewhere.contains(name)) {
        if (why) *why = "another file of the program names it";
        return true;
    }
    return false;
}

void QueueOwnershipAnalysisPass::poisonEscapes() {
    // The shim's queue types that carry a verdict; a library's queues are
    // never rewritten, and their verdicts say what may use them out of
    // sight instead (openWhy).
    llvm::SetVector<Type> types;
    for (auto &entry : roots) {
        if (libraryKeys.contains(entry.first)) continue;
        if (Type t = queueTypeOf(entry.first); t && !poisonedTypes.contains(t)) types.insert(t);
    }
    if (types.empty()) return;
    // The shim's own code, and the standard library's, which keep their
    // queues to themselves; a callback handed to a library is a root of
    // the walk already.
    auto shimOrStd = [](llvm::StringRef n) {
        return n.starts_with("_ZN3wtc") || n.starts_with("_ZNK3wtc") || n.starts_with("_ZSt") ||
               n.starts_with("_ZNSt") || n.starts_with("_ZNKSt");
    };
    // Functions with no body here that cannot hand a queue to anyone: they
    // touch only the bytes they are handed, allocate or free them, start
    // a thread whose entry the walk follows, or run a destructor at exit.
    auto harmless = [&](cir::FuncOp callee) {
        const llvm::StringRef n = callee.getSymName();
        return getSemanticKind(callee) != SemanticOpKind::Unknown || isLibraryFunction(callee) ||
               isAllocator(callee) || touchesOnlyItsArguments(callee) || isCQueueRuntime(callee) || n == "free" ||
               n.starts_with("_ZdlPv") || n.starts_with("_ZdaPv") || n == "__cxa_atexit" ||
               n == "__cxa_thread_atexit" || n == "__cxa_thread_atexit_impl" || shimOrStd(n);
    };
    for (Type t : types) {
        // Whether a value of this type is, holds, or points to a queue of
        // type t, at any depth: a pointer to it, a structure of parameters
        // naming it, a vector of pointers to it. True answers are kept; a
        // false one may come from a type still being looked at on a cycle.
        llvm::DenseSet<Type> holding;
        std::function<bool(Type, llvm::DenseSet<Type> &)> holdsIn = [&](Type ty, llvm::DenseSet<Type> &seen) {
            while (auto a = dyn_cast_or_null<cir::ArrayType>(ty)) ty = a.getElementType();
            if (!ty) return false;
            if (ty == t || holding.contains(ty)) return true;
            if (!seen.insert(ty).second) return false;
            bool r = false;
            if (auto p = dyn_cast<cir::PointerType>(ty)) r = holdsIn(p.getPointee(), seen);
            else if (auto rec = dyn_cast<cir::RecordType>(ty); rec && !rec.isIncomplete())
                r = llvm::any_of(rec.getMembers(), [&](Type m) { return holdsIn(m, seen); });
            if (r) holding.insert(ty);
            return r;
        };
        // A whole search from one type is exact, and kept.
        llvm::DenseMap<Type, bool> holdsMemo;
        auto holds = [&](Type ty) {
            if (auto it = holdsMemo.find(ty); it != holdsMemo.end()) return it->second;
            llvm::DenseSet<Type> seen;
            const bool r = holdsIn(ty, seen);
            holdsMemo[ty] = r;
            return r;
        };
        // Whether a value carries the address of such a queue: its type
        // says so, at any depth (a structure of parameters passed by
        // value); or, seen through casts of any kind -- (void *)&q,
        // (uintptr_t)&q -- what it was made from does; or it is loaded
        // from, or points into, an object such a value was stored in (a
        // local variable, a void * member set to &q, the copy the ABI makes
        // of a structure passed by value); or it is a parameter some caller
        // here passes such a value for.
        llvm::DenseSet<Operation *> taintedRoots;   // allocas, globals, allocating calls
        std::optional<llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>>> callersOf;
        auto rootOf = [](Value a) {
            a = traceToLocalRoot(a);
            for (unsigned depth = 0; depth < 8; ++depth) {
                if (auto m = a.getDefiningOp<cir::GetMemberOp>()) a = traceToLocalRoot(m.getAddr());
                else if (auto ps = a.getDefiningOp<cir::PtrStrideOp>()) a = traceToLocalRoot(ps.getBase());
                else if (auto ge = a.getDefiningOp<cir::GetElementOp>()) a = traceToLocalRoot(ge.getBase());
                else break;
            }
            return a;
        };
        // The callers here of each function, for its parameters.
        auto callersOfFunc = [&](cir::FuncOp f) -> llvm::ArrayRef<cir::CallOp> {
            if (!callersOf) {
                callersOf.emplace();
                module.walk([&](cir::CallOp c) {
                    if (cir::FuncOp to = lookupCallee(c); to && !c.isIndirect())
                        (*callersOf)[to.getOperation()].push_back(c);
                });
            }
            auto it = callersOf->find(f.getOperation());
            return it == callersOf->end() ? llvm::ArrayRef<cir::CallOp>() : llvm::ArrayRef<cir::CallOp>(it->second);
        };
        // The objects a pointer may point into: a local, a global or a heap
        // object made by a call; through a pointer kept in a local slot (a
        // parameter spilled at -O0, `box *b = new box`), every value stored
        // in the slot; through a parameter, what each direct caller passes.
        // A pointer loaded from anywhere else points nowhere this knows.
        std::function<void(Value, unsigned, llvm::SmallVectorImpl<Operation *> &)> pointees =
            [&](Value addr, unsigned depth, llvm::SmallVectorImpl<Operation *> &out) {
                if (depth > 8) return;
                Value a = rootOf(addr);
                if (auto arg = dyn_cast<BlockArgument>(a)) {
                    auto f = dyn_cast<cir::FuncOp>(arg.getOwner()->getParentOp());
                    if (!f || arg.getOwner() != &f.getBody().front()) return;
                    for (cir::CallOp call : callersOfFunc(f)) {
                        auto args = call.getArgOperands();
                        if (arg.getArgNumber() < args.size()) pointees(args[arg.getArgNumber()], depth + 1, out);
                    }
                    return;
                }
                Operation *d = a.getDefiningOp();
                if (!d) return;
                if (auto gg = dyn_cast<cir::GetGlobalOp>(d)) {
                    if (auto g = module.lookupSymbol<cir::GlobalOp>(gg.getName())) out.push_back(g);
                } else if (isa<cir::AllocaOp, cir::CallOp>(d)) {
                    out.push_back(d);
                } else if (auto ld = dyn_cast<cir::LoadOp>(d)) {
                    if (auto slot = traceToLocalRoot(ld.getAddr()).getDefiningOp<cir::AllocaOp>())
                        for (Operation *user : slot->getUsers())
                            if (auto st = dyn_cast<cir::StoreOp>(user); st && st.getAddr() == slot.getResult())
                                pointees(st.getValue(), depth + 1, out);
                }
            };
        // An object one of whose bytes may hold such an address.
        auto tainted = [&](Value addr) {
            llvm::SmallVector<Operation *, 4> objects;
            pointees(addr, 0, objects);
            return llvm::any_of(objects, [&](Operation *o) { return taintedRoots.contains(o); });
        };
        std::function<bool(Value, unsigned)> carries = [&](Value v, unsigned depth) -> bool {
            if (depth > 8) return false;
            if (holds(v.getType())) return true;
            Value r = v;
            while (Operation *d = r.getDefiningOp()) {
                if (auto c = dyn_cast<cir::CastOp>(d)) r = c.getSrc();
                else if (auto u = dyn_cast<mlir::UnrealizedConversionCastOp>(d); u && u.getNumOperands() == 1)
                    r = u.getOperand(0);
                else break;
                if (holds(r.getType())) return true;
            }
            // A pointer into such an object, or a value read from one: a
            // field the address was stored in, the copy the ABI makes of a
            // structure passed by value.
            if (isa<cir::PointerType>(r.getType()) && tainted(r)) return true;
            if (auto ld = r.getDefiningOp<cir::LoadOp>(); ld && tainted(ld.getAddr())) return true;
            // A parameter: what any direct caller here passes for it.
            if (auto arg = dyn_cast<BlockArgument>(r)) {
                auto f = dyn_cast<cir::FuncOp>(arg.getOwner()->getParentOp());
                if (!f || arg.getOwner() != &f.getBody().front()) return false;
                for (cir::CallOp call : callersOfFunc(f)) {
                    auto args = call.getArgOperands();
                    if (arg.getArgNumber() < args.size() && carries(args[arg.getArgNumber()], depth + 1))
                        return true;
                }
            }
            return false;
        };
        // Every object the address may point into, a store through a
        // pointer parameter included (`*slot = &q` taints what the callers
        // pass for slot).
        auto taint = [&](Value addr) {
            llvm::SmallVector<Operation *, 4> objects;
            pointees(addr, 0, objects);
            bool grew = false;
            for (Operation *o : objects) grew |= taintedRoots.insert(o).second;
            return grew;
        };
        // The objects such an address is stored or copied into, until
        // nothing changes: a global whose initialiser names a queue (the
        // constant a local structure is copied from), a local, a field or a
        // heap object given such a value, an object copied from a tainted
        // one.
        module.walk([&](cir::GlobalOp g) {
            std::optional<Attribute> init = g.getInitialValue();
            if (!init) return;
            bool names = false;
            init->walk([&](cir::GlobalViewAttr gv) {
                auto viewed = module.lookupSymbol<cir::GlobalOp>(gv.getSymbol().getValue());
                if (viewed && holds(viewed.getSymType())) names = true;
            });
            if (names) taintedRoots.insert(g);
        });
        // A fixpoint: the set only grows, and is bounded by the objects of
        // the module, so a chain of copies written in any order is
        // followed to its end.
        for (bool changed = true; changed;) {
            changed = false;
            module.walk([&](Operation *op) {
                if (auto st = dyn_cast<cir::StoreOp>(op)) {
                    if (!tainted(st.getAddr()) && carries(st.getValue(), 0)) changed |= taint(st.getAddr());
                } else if (auto cp = dyn_cast<cir::CopyOp>(op)) {
                    if (!tainted(cp.getDst()) && carries(cp.getSrc(), 0)) changed |= taint(cp.getDst());
                }
            });
        }
        auto reaches = [&](Value v) { return carries(v, 0); };
        Operation *at = nullptr;
        std::string why;
        // Handed to code with no body here: another file, or a library.
        // Directly, as an argument of a function with no body; along with
        // the address of such a function -- a thread started in it
        // (wtc::spawn, std::thread, pthread_create) or a callback handed
        // with its argument; or through a call by pointer, which reaches
        // such a function when the program takes the address of one, and
        // anything when other units may hand it theirs.
        auto externalEntry = [&](Value v) -> cir::FuncOp {
            Value r = v;
            while (Operation *d = r.getDefiningOp()) {
                if (auto c = dyn_cast<cir::CastOp>(d)) r = c.getSrc();
                else if (auto u = dyn_cast<mlir::UnrealizedConversionCastOp>(d); u && u.getNumOperands() == 1)
                    r = u.getOperand(0);
                else break;
            }
            auto gg = r.getDefiningOp<cir::GetGlobalOp>();
            auto f = gg ? module.lookupSymbol<cir::FuncOp>(gg.getName()) : cir::FuncOp();
            return f && f.getBody().empty() && !harmless(f) ? f : cir::FuncOp();
        };
        auto inProgram = [&](Operation *op) {
            auto in = op->getParentOfType<cir::FuncOp>();
            return in && !isLibraryFunction(in) && !shimOrStd(in.getSymName()) &&
                   !in.getSymName().starts_with("__") && getSemanticKind(in) == SemanticOpKind::Unknown;
        };
        cir::FuncOp takenOutside;
        module.walk([&](cir::GetGlobalOp gg) {
            if (takenOutside || !inProgram(gg)) return;
            auto f = module.lookupSymbol<cir::FuncOp>(gg.getName());
            if (f && f.getBody().empty() && !harmless(f) && !f.getSymName().starts_with("__")) takenOutside = f;
        });
        module.walk([&](cir::CallOp call) {
            if (!inProgram(call)) return WalkResult::advance();
            cir::FuncOp callee = lookupCallee(call);
            auto args = call.getArgOperands();
            if (callee && callee.getBody().empty() && !harmless(callee) && llvm::any_of(args, reaches)) {
                at = call;
                why = "it is handed to " + llvm::demangle(callee.getSymName().str()) + ", whose body is not here";
                return WalkResult::interrupt();
            }
            for (Value a : call.getOperands())
                if (cir::FuncOp entry = externalEntry(a); entry && llvm::any_of(args, reaches)) {
                    at = call;
                    why = "it is handed, with the address of " + llvm::demangle(entry.getSymName().str()) +
                          ", whose body is not here, to " +
                          (callee ? llvm::demangle(callee.getSymName().str()) : std::string("a call through a pointer"));
                    return WalkResult::interrupt();
                }
            if (!callee && call.isIndirect() && (openWorld || !namedElsewhere.empty() || takenOutside) &&
                llvm::any_of(args, reaches)) {
                at = call;
                why = takenOutside ? "it is handed to a call through a pointer, which may reach " +
                                         llvm::demangle(takenOutside.getSymName().str()) +
                                         ", whose body is not here"
                                   : std::string("it is handed to a call through a pointer, which may reach "
                                                 "another translation unit");
                return WalkResult::interrupt();
            }
            return WalkResult::advance();
        });
        // Named by another unit: a global it may name that holds the queue
        // or a pointer to it, or into which its address is stored.
        if (!at)
            module.walk([&](cir::GlobalOp g) {
                std::string elsewhere;
                if (!holds(g.getSymType()) || !visibleElsewhere(g, &elsewhere)) return WalkResult::advance();
                at = g;
                why = "it is reachable from the global " + llvm::demangle(g.getSymName().str()) + ", and " +
                      elsewhere;
                return WalkResult::interrupt();
            });
        if (!at && (openWorld || !namedElsewhere.empty()))
            module.walk([&](cir::StoreOp st) {
                if (!reaches(st.getValue())) return WalkResult::advance();
                Value a = traceToLocalRoot(st.getAddr());
                for (unsigned depth = 0; depth < 8; ++depth) {
                    if (auto m = a.getDefiningOp<cir::GetMemberOp>()) a = traceToLocalRoot(m.getAddr());
                    else if (auto ps = a.getDefiningOp<cir::PtrStrideOp>()) a = traceToLocalRoot(ps.getBase());
                    else break;
                }
                auto gg = a.getDefiningOp<cir::GetGlobalOp>();
                auto g = gg ? module.lookupSymbol<cir::GlobalOp>(gg.getName()) : cir::GlobalOp();
                std::string elsewhere;
                if (!g || !visibleElsewhere(g, &elsewhere)) return WalkResult::advance();
                at = st;
                why = "its address is stored in the global " + llvm::demangle(g.getSymName().str()) + ", and " +
                      elsewhere;
                return WalkResult::interrupt();
            });
        // Handed out by a function another unit may call: returned, or
        // written through a parameter (a pointer to a pointer to it). A
        // parameter that is the queue itself is no way out -- the other
        // unit would hand in a queue of its own, and an access through a
        // parameter of a root is unresolvable anyway. Inline functions and
        // template instances are each unit's own copy, and hand out only
        // what their callers hand them.
        if (!at && (openWorld || !namedElsewhere.empty()))
            module.walk([&](cir::FuncOp f) {
                if (f.getBody().empty() || f.getSymName() == "main" || shimOrStd(f.getSymName()) ||
                    f.getLinkage() != cir::GlobalLinkageKind::ExternalLinkage)
                    return WalkResult::advance();
                std::string elsewhere;
                if (!visibleElsewhere(f, &elsewhere)) return WalkResult::advance();
                auto fty = f.getFunctionType();
                bool out = !fty.hasVoidReturn() && holds(fty.getReturnType());
                for (Type in : fty.getInputs()) {
                    auto p = dyn_cast<cir::PointerType>(in);
                    if (p && p.getPointee() != t && holds(p.getPointee())) out = true;
                }
                if (!out) return WalkResult::advance();
                at = f;
                why = "it is handed out by " + llvm::demangle(f.getSymName().str()) + ", and " + elsewhere;
                return WalkResult::interrupt();
            });
        if (!at) continue;
        poisonedTypes.insert(t);
        poisonWhy[t] = "it escapes this translation unit: " + why;
        at->emitRemark() << "wtc: a queue escapes this translation unit (" << why
                         << "); every queue of its type is now unknown";
        ++remarks;
    }
}

bool QueueOwnershipAnalysisPass::checkDeclared(Operation *root, const RootInfo &info, Topology verdict,
                                               const std::string &label,
                                               llvm::ArrayRef<cir::CallOp> ctors, bool capExceeded,
                                               const char *summary, bool library, Type objTy) {
    std::string how, open;
    std::optional<int64_t> tag;
    // Where a library queue's diagnostics go: its constructor, or with none
    // seen, the queue itself.
    Operation *at = ctors.empty() ? root : cir::CallOp(ctors.front()).getOperation();
    if (!library) {
        tag = constructorTag(ctors);
    } else {
        LibraryDeclaration d = libraryDeclared(ctors, info);
        open = openWhy(objTy);
        how = d.how;
        const bool manyProducers = verdict == Topology::mpsc || verdict == Topology::mpmc ||
                                   verdict == Topology::unknown;
        const bool manyConsumers = verdict == Topology::spmc || verdict == Topology::mpmc ||
                                   verdict == Topology::unknown;
        // A type whose queues are all unknown is past checking whatever is
        // declared; say why, as for a shim queue below.
        auto poisoned = poisonWhy.find(objTy);
        // A side whose declaration is not known is checked only when this
        // translation unit has at most one thread on it.
        if (poisoned == poisonWhy.end() && ((d.producers == Declared::Unknown && manyProducers) ||
                                            (d.consumers == Declared::Unknown && manyConsumers))) {
            at->emitWarning() << "wtc: queue " << label << " cannot be checked: " << d.unknown;
            ++remarks;
            return true;
        }
        const bool oneProducer = d.producers == Declared::One, oneConsumer = d.consumers == Declared::One;
        tag = oneProducer && oneConsumer ? 1 : oneConsumer ? 3 : oneProducer ? 4 : 0;
        // A queue that declares nothing single has nothing to check: a
        // pool on the default ops, a ring created with no flags.
        // Unless the type has a single-thread form whose calls may be the
        // unknown ones: a ring created with no flags is single-producer
        // where rte_ring_sp_enqueue is called through its address.
        const bool declaresNothing =
            d.producers == Declared::Many && d.consumers == Declared::Many && !hasSingleForm(objTy);
        if (*tag == 0 && poisoned != poisonWhy.end() && !declaresNothing) {
            at->emitWarning() << "wtc: queue " << label << " cannot be checked: " << poisoned->second;
            ++remarks;
            return true;
        }
        // Declared for many on both sides: more cautious than needed when
        // the threads allow less, unless code out of sight may use it too.
        if (*tag == 0) {
            if (d.producers == Declared::Many && d.consumers == Declared::Many && open.empty() &&
                (verdict == Topology::spsc || verdict == Topology::mpsc || verdict == Topology::spmc)) {
                at->emitRemark() << "wtc: queue " << label
                                 << " is declared mpmc, but in this translation unit its threads allow "
                                 << stringifyTopology(verdict);
                ++remarks;
            }
            return true;
        }
    }
    if (!tag || *tag == 0) return true;  // nothing declared, or nothing readable
    // mpsc promises one consumer and any number of producers, spmc one
    // producer and any number of consumers; the others, one of each.
    const bool declaredMpsc = *tag == 3, declaredSpmc = *tag == 4;
    std::string declared = *tag == 2       ? "spsc_inline"
                           : declaredMpsc ? "mpsc"
                           : declaredSpmc ? "spmc"
                                          : "spsc";
    if (!how.empty()) declared += " (" + how + ")";
    if (verdict == Topology::spsc || (declaredMpsc && verdict == Topology::mpsc) ||
        (declaredSpmc && verdict == Topology::spmc)) {
        // A side declared single with no access at all holds vacuously:
        // in this translation unit nothing touches it, so nothing was
        // checked there. mpsc declares only its consumer single, spmc only
        // its producer.
        const bool producerSingle = *tag != 3, consumerSingle = *tag != 4;
        const bool noProducer = producerSingle && info.producers.accesses.empty();
        const bool noConsumer = consumerSingle && info.consumers.accesses.empty();
        InFlightDiagnostic r = at->emitRemark() << "wtc: queue " << label << ": declared " << declared << ", ";
        if (vacuousOutcome && (noProducer || noConsumer)) {
            const char *none = noProducer && noConsumer ? "producer or consumer" : noProducer ? "producer" : "consumer";
            // Both sides declared single and one of them seen: that one
            // is verified.
            if (producerSingle && consumerSingle && !(noProducer && noConsumer))
                r << "verified for its " << (noProducer ? "consumer" : "producer") << " and ";
            r << "vacuously satisfied: no " << none << " access in this translation unit";
            if (library && !open.empty()) r << "; " << open;
        } else {
            r << "verified";
            // A library queue may be shared with code the module does not
            // show: another file, another process, a library that takes it
            // whole or inside a structure. The verdict is about this one.
            if (library) r << " in this translation unit" << (open.empty() ? "" : "; " + open);
        }
        ++remarks;
        return true;
    }
    // Certainly broken: on one side, two threads the module accounts for
    // that can both exist, or one that certainly stands for several.
    const char *exclusiveSide = nullptr, *conditionalSide = nullptr;
    for (const SideInfo *side : {&info.producers, &info.consumers}) {
        if (summary) break;
        const bool pushes = side == &info.producers;
        if ((pushes && declaredMpsc) || (!pushes && declaredSpmc)) continue;
        llvm::SmallVector<const Access *, 4> firstOfThread;
        llvm::SmallSet<unsigned, 4> seen;
        bool certainMany = false;
        for (const Access &a : side->accesses) {
            if (a.speculative || a.oneOf) continue;
            if (a.many) certainMany = true;
            if (seen.insert(a.thread).second) firstOfThread.push_back(&a);
        }
        // Threads started on different arms of one branch cannot both
        // exist (ThreadInstanceWalk::exclusive), and two of those are no
        // proof of two producers.
        // Two threads each started under a condition of its own may be
        // exclusive too, by conditions the walk does not compare -- `if (!f)
        // spawn(a); ... if (f) spawn(b);` -- and are no proof either.
        bool coexist = false, conditional = false;
        for (size_t i = 0; i < firstOfThread.size() && !coexist; ++i)
            for (size_t j = i + 1; j < firstOfThread.size() && !coexist; ++j) {
                const unsigned ta = firstOfThread[i]->thread, tb = firstOfThread[j]->thread;
                if (walk->exclusive(ta, tb)) continue;
                if (walk->bothConditional(ta, tb)) { conditional = true; continue; }
                coexist = true;
            }
        if (!certainMany && !coexist) {
            if (firstOfThread.size() >= 2) {
                const char *what = pushes ? "push to it" : "pop from it";
                (conditional ? conditionalSide : exclusiveSide) = what;
            }
            continue;
        }
        InFlightDiagnostic d = at->emitError()
                               << "wtc: queue " << label << " is declared " << declared << " but ";
        if (certainMany) d << "many threads ";
        else d << firstOfThread.size() << " threads ";
        d << (pushes ? "push to it" : "pop from it");
        for (const Access *a : firstOfThread) {
            Diagnostic &n = d.attachNote(a->op->getLoc());
            n << (pushes ? "push" : "pop") << " on a thread that starts in " << a->entry;
            if (a->many) n << ", one of several started";
            if (a->spawned) {
                std::string w = whereOf(a->started);
                n << " at " << (w.empty() ? std::string("a spawn") : w);
            }
            else n << " (the program's own thread)";
        }
        return false;
    }
    // Not as declared, but nothing certain: say why it could not be checked.
    InFlightDiagnostic w = at->emitWarning()
                           << "wtc: queue " << label << " is declared " << declared << "; cannot verify: ";
    if (!objTy) {
        cir::CallOp first = ctors.empty() ? cir::CallOp() : cir::CallOp(ctors.front());
        auto built = first ? dyn_cast<cir::PointerType>(first.getOperand(0).getType()) : cir::PointerType();
        objTy = built ? built.getPointee() : queueObjectType(root);
    }
    if (capExceeded) w << "the program has too many function instances";
    else if (globalPoison) w << "an access to some queue could not be attributed";
    else if (auto it = poisonWhy.find(objTy); it != poisonWhy.end()) w << it->second;
    else if (summary) w << summary;
    else if (const Access *u = unresolvedPutOf(info.producers); u && !declaredMpsc && countOf(info.producers) == 2)
        w << u->poolWhy;
    else if (llvm::any_of(info.producers.accesses, [](const Access &a) { return a.oneOf && a.poolWhy.empty(); }) ||
             llvm::any_of(info.consumers.accesses, [](const Access &a) { return a.oneOf; }))
        w << "an operation may act on it or on another queue";
    else if (conditionalSide)
        w << "the threads that " << conditionalSide << " are each started under a condition of its own, "
          << "and whether the conditions can hold together is not known";
    else if (exclusiveSide)
        w << "the threads that " << exclusiveSide << " are started on different arms of one branch, so "
          << "at most one of them exists, which the count does not tell";
    else w << "a thread that uses it is one the module cannot account for (its caller is not visible)";
    return true;
}

void QueueOwnershipAnalysisPass::writeVerdicts(bool capExceeded) {
    MLIRContext *ctx = &getContext();
    llvm::DenseMap<Key, Topology> verdictOf;
    auto ctorsOf = collectQueueConstructors(module);
    bool contractsHold = true;

    for (auto &[key, info] : roots) {
        Topology t;
        const unsigned p = countOf(info.producers), c = countOf(info.consumers);
        const Type objTy = queueTypeOf(key);
        // An array whose constructors build queues of different types has
        // no type to check the poisoned ones against.
        if (globalPoison || capExceeded || (objTy && poisonedTypes.count(objTy)) ||
            (key.second == kElementsOffset && !objTy))
            t = Topology::unknown;
        else if (p <= 1 && c <= 1)
            t = Topology::spsc;   // one side empty is still safe for an SPSC ring
        else if (p >= 2 && c <= 1)
            t = Topology::mpsc;
        else if (p <= 1 && c >= 2)
            t = Topology::spmc;
        else
            t = Topology::mpmc;
        verdictOf[key] = t;

        Operation *root = key.first;
        const bool whole = isWhole(key, objTy);
        const bool library = libraryKeys.contains(key);
        const std::string label = labelOf(key, whole);
        const char *noCarrier = nullptr;
        Operation *carrier = library ? nullptr : whole ? root : carrierOf(key, noCarrier);
        if (carrier) {
            carrier->setAttr(kTopologyAttrName, TopologyAttr::get(ctx, t));
            carrier->setAttr(kQueueIdAttrName, IntegerAttr::get(IntegerType::get(ctx, 64), idOf(key)));
            if (!whole) carrier->setAttr(kQueueNameAttrName, StringAttr::get(ctx, label));
        }
        root->emitRemark() << "wtc: queue " << label << ": " << stringifyTopology(t)
                           << " (producers: " << (p == 2 ? "many" : std::to_string(p))
                           << ", consumers: " << (c == 2 ? "many" : std::to_string(c)) << ")";
        ++remarks;
        // A side counted as one although several threads touch it.
        for (auto [n, side, what] : {std::tuple{p, &info.producers, "producers"},
                                     std::tuple{c, &info.consumers, "consumers"}})
            if (n == 1 && side->count() == 2) {
                root->emitRemark() << "wtc: queue " << label << ": " << what << ": "
                                   << side->threads.size()
                                   << " threads, started on different arms of one branch, so at most one exists";
                ++remarks;
            }
        // What a programmer would change to get a smaller count.
        for (auto [n, side, what] : {std::tuple{p, &info.producers, "producers"},
                                     std::tuple{c, &info.consumers, "consumers"}})
            if (n == 2)
                if (std::string w = whyMany(*side); !w.empty()) {
                    root->emitRemark() << "wtc: queue " << label << ": many " << what << ": " << w;
                    ++remarks;
                }
        // A side that is many only because one counted loop starts its
        // threads has one thread when the loop runs at most once, which the
        // program knows before the loop runs: the loop is recorded for the
        // specialisation pass, which can choose the implementation then.
        if (carrier && t != Topology::unknown)
            for (auto [n, side, what, attr] :
                 {std::tuple{p, &info.producers, "producers", kProducersLoopAttrName},
                  std::tuple{c, &info.consumers, "consumers", kConsumersLoopAttrName}}) {
                Operation *loop = n == 2 ? side->countLoop() : nullptr;
                if (!loop) continue;
                auto [it, fresh] = loopIds.try_emplace(loop, static_cast<int64_t>(loopIds.size()));
                if (fresh)
                    loop->setAttr(kSpawnLoopAttrName, IntegerAttr::get(IntegerType::get(ctx, 64), it->second));
                carrier->setAttr(attr, IntegerAttr::get(IntegerType::get(ctx, 64), it->second));
                root->emitRemark() << "wtc: queue " << label << ": " << what
                                   << ": one thread per iteration of the loop at "
                                   << whereOf(loop->getLoc(), /*fileOnly=*/true)
                                   << ", so one when it runs at most once";
                ++remarks;
            }
        if (!carrier && !library && t == Topology::spsc) {
            root->emitRemark() << "wtc: queue " << label << " keeps the general-purpose implementation: "
                               << noCarrier;
            ++remarks;
        }
        llvm::SmallVector<cir::CallOp, 2> ctors;
        if (whole && !library) {
            if (auto it = ctorsOf.find(root); it != ctorsOf.end()) ctors.assign(it->second.begin(), it->second.end());
        } else if (auto it = keyCtors.find(key); it != keyCtors.end()) {
            for (Operation *c : it->second) ctors.push_back(cast<cir::CallOp>(c));
        }
        // A library queue declares through its calls too, so it is checked
        // even when its constructor is out of sight.
        if (!ctors.empty() || library) {
            const char *summary =
                key.second == kElementsOffset
                    ? "it stands for every element of an array, counted together, and each element may have "
                      "threads of its own"
                : isa<cir::CallOp>(root) && !walk->siteRunsOnce(root)
                    ? "it is one of the objects one allocation site makes, and the threads may each have their own"
                    : nullptr;
            contractsHold &= checkDeclared(root, info, t, label, ctors, capExceeded, summary, library,
                                           library ? objTy : Type());
        }
    }
    if (!contractsHold) signalPassFailure();

    // Each operation gets the meet over every queue it may act on; a
    // library's calls stay as the program wrote them.
    for (auto &[op, ks] : opRoots) {
        if (libraryOp(op)) continue;
        Topology t = Topology::unknown;
        bool first = true;
        for (const Key &k : ks) {
            Topology kt = verdictOf.lookup(k);
            if (first) { t = kt; first = false; }
            else if (kt != t) { t = Topology::unknown; }
        }
        op->setAttr("topology", TopologyAttr::get(ctx, t));
        // An operation that may act on several queues has no queue_id: a
        // later pass reading it would treat the others' operations as
        // unrelated to this one. It lists them all instead.
        if (ks.size() != 1) {
            llvm::SmallVector<int64_t, 4> ids;
            for (const Key &k : ks) ids.push_back(idOf(k));
            llvm::sort(ids);
            op->setAttr(kQueueIdsAttrName, DenseI64ArrayAttr::get(ctx, ids));
            continue;
        }
        op->setAttr("queue_id", IntegerAttr::get(IntegerType::get(ctx, 64), idOf(ks.front())));
    }
    // Poisoned operations that resolved to nothing: unknown, explicitly.
    module.walk([&](Operation *op) {
        if (isQueueOp(op) && !libraryOp(op) && !op->getAttr("topology"))
            op->setAttr("topology", TopologyAttr::get(ctx, Topology::unknown));
    });
}

void QueueOwnershipAnalysisPass::runOnOperation() {
    module = getOperation();
    roots.clear(); poisonedTypes.clear(); poisonWhy.clear(); opRoots.clear();
    ctorKeys.clear(); keyCtors.clear(); ctorUnplaced.clear(); keyType.clear();
    memberCtors.clear(); abstractKeys.clear(); elementKeys.clear(); layout.reset();
    keyCtorInst.clear(); captureStores.clear(); libraryKeys.clear(); openWhyCache.clear();
    libraryFunctions.clear(); unresolvedPuts.clear(); mbufPoolKeys.clear(); mbufLink.reset();
    defaultOpsSet.reset(); objectPools.clear(); singleForm.clear();
    globalPoison = false; remarks = 0;
    namedElsewhere.clear();
    openWorld = openWorldOption;
    openWorldWhy = "the unit is built to be linked with other files of the program";
    // Without main the rest of the program is elsewhere, and may name
    // anything, unless the files that make it up are listed: then they
    // name what the list says.
    if (!openWorld && namedElsewhereFile.empty()) {
        auto mainFn = module.lookupSymbol<cir::FuncOp>("main");
        if (!mainFn || mainFn.getBody().empty()) {
            openWorld = true;
            openWorldWhy = "this unit has no main, so the rest of the program is elsewhere";
        }
    }
    if (!namedElsewhereFile.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(namedElsewhereFile.getValue());
        if (!buf) {
            module.emitError() << "wtc: cannot read the symbols named elsewhere from " << namedElsewhereFile.getValue();
            return signalPassFailure();
        }
        llvm::SmallVector<llvm::StringRef, 64> lines;
        (*buf)->getBuffer().split(lines, '\n', -1, false);
        for (llvm::StringRef l : lines)
            if (!(l = l.trim()).empty()) namedElsewhere.insert(l);
    }
    poisonedOps.clear();

    // A function that can reach neither a queue operation nor a queue's
    // constructor cannot affect a verdict, nor which call builds a queue.
    // The predicate needs the walk that owns the memo table, and the walk
    // needs the predicate, so the lambda is named: a temporary would die at
    // the end of the constructor call and leave function_ref dangling.
    ThreadInstanceWalk *self = nullptr;
    // A modelled library's bodies are walked like any other when they reach
    // the program's code (a functor handed to consume_one, an element's
    // constructor inside emplace), which may use queues of its own. The
    // modelled operations inside them are not counted (inLibraryBody), nor
    // do they make a body worth entering: DPDK's chains of inline ring
    // functions reach nothing of the program's.
    auto relevant = [&](cir::FuncOp f) {
        return self->reachesOp(f, [this](Operation *op) {
            return isQueueOpOrConstructor(op) && !inLibraryBody(op);
        });
    };
    ThreadInstanceWalk walk(module, relevant);
    // A function another unit may call runs on threads this one cannot
    // count; main is the program's own, run once. An inline function or a
    // template instance (linkonce) is each unit's own copy: what another
    // unit's copy touches is its own, or a queue that unit could name,
    // which escapes (poisonEscapes).
    if (openWorld || !namedElsewhere.empty())
        walk.setCallableElsewhere([this](cir::FuncOp f) {
            const auto l = f.getLinkage();
            return f.getSymName() != "main" &&
                   (l == cir::GlobalLinkageKind::ExternalLinkage || l == cir::GlobalLinkageKind::WeakAnyLinkage ||
                    l == cir::GlobalLinkageKind::WeakODRLinkage) &&
                   visibleElsewhere(f);
        });
    self = &walk;
    this->walk = &walk;

    // Every queue operation under every instance, resolved once the walk
    // is over: a pointer loaded from a field is placed under each instance
    // of every function that stores it, and in the middle of the walk some
    // of those instances do not exist yet.
    llvm::SmallVector<std::pair<Operation *, Instance *>, 32> visits, ctorVisits;
    walk.run(
        [&](Instance &inst) {
            inst.func.walk([&](Operation *op) {
                if (inLibraryBody(op)) return;
                if (isQueueOp(op)) visits.push_back({op, &inst});
                else if (isQueueConstructor(op)) ctorVisits.push_back({op, &inst});
            });
        },
        [&](cir::FuncOp f) { poisonReachable(f); });

    // What each constructor call builds. A queue inside a struct is built
    // by the struct's constructor, under the instance that constructs that
    // object; a heap queue by the call after its allocation.
    for (auto [op, inst] : ctorVisits) {
        auto call = cast<cir::CallOp>(op);
        // rte_ring_create is the allocation and the constructor in one: the
        // ring is the object the call hands back, which the walk places at
        // the call itself.
        if (isLibraryCreate(op)) {
            auto made = call.getNumResults() ? dyn_cast<cir::PointerType>(call.getResult().getType())
                                             : cir::PointerType();
            if (!made) { ctorUnplaced.insert(op); continue; }
            const Key k{op, 0};
            ctorKeys[op].insert(k);
            keyCtors[k].insert(op);
            keyType.try_emplace(k, made.getPointee());
            auto [ci, first] = keyCtorInst.try_emplace(k, inst);
            if (!first && ci->second != inst) ci->second = kAmbiguous;
            if (libraryCall(call).kind == LibraryQueueKind::Mempool) {
                if (mayHoldMbufs(call)) mbufPoolKeys.insert(k);
                else objectPools.insert(k);
            }
            continue;
        }
        auto built = dyn_cast<cir::PointerType>(call.getOperand(0).getType());
        // A library queue built where the walk cannot name the object --
        // rte_ring_init on an element of an array, say -- leaves its
        // declaration unread wherever its pushes and pops land: every
        // queue of its type is unknown, rather than undeclared.
        const bool library = static_cast<bool>(libraryCall(call));
        // An array of queues the front end builds element by element.
        if (built) {
            llvm::SmallVector<Operation *, 2> arrays = arrayCtorRoots(call.getOperand(0), inst);
            for (Operation *root : arrays) {
                const Key k{root, kElementsOffset};
                elementKeys.insert(k);
                ctorKeys[op].insert(k);
                keyCtors[k].insert(op);
                auto [it, fresh] = keyType.try_emplace(k, built.getPointee());
                if (!fresh && it->second != built.getPointee()) it->second = Type();
                auto [ci, first] = keyCtorInst.try_emplace(k, inst);
                if (!first && ci->second != inst) ci->second = kAmbiguous;
            }
            if (!arrays.empty()) continue;
        }
        for (const Place &pl : walk.resolvePlaces(call.getOperand(0), inst)) {
            // An element of an array, built with an index the walk cannot
            // read -- `new (&qs[i]) queue` in a loop: every element.
            if (built && pl.root && (pl.unknownIndex || pl.loop)) {
                const Key k{pl.root, kElementsOffset};
                elementKeys.insert(k);
                ctorKeys[op].insert(k);
                keyCtors[k].insert(op);
                auto [it, fresh] = keyType.try_emplace(k, built.getPointee());
                if (!fresh && it->second != built.getPointee()) it->second = Type();
                auto [ci, first] = keyCtorInst.try_emplace(k, inst);
                if (!first && ci->second != inst) ci->second = kAmbiguous;
                continue;
            }
            if (!built || !pl.root || pl.unknownIndex || pl.loop) {
                ctorUnplaced.insert(op);
                if (library && built && poisonedTypes.insert(built.getPointee()).second) {
                    poisonWhy[built.getPointee()] =
                        "a queue of its type is built where the analysis cannot name the object";
                    call.emitRemark() << "wtc: this queue is built where the analysis cannot name the "
                                         "object; every queue of its type is now unknown";
                    ++remarks;
                }
                continue;
            }
            const Key k{pl.root, pl.offset};
            ctorKeys[op].insert(k);
            keyCtors[k].insert(op);
            // A pool made out of sight, set up here: it may hold mbufs.
            if (library && libraryCall(call).kind == LibraryQueueKind::Mempool && !isLibraryCreate(pl.root))
                mbufPoolKeys.insert(k);
            auto [it, fresh] = keyType.try_emplace(k, built.getPointee());
            if (!fresh && it->second != built.getPointee()) it->second = Type();   // two types: neither
            auto [ci, first] = keyCtorInst.try_emplace(k, inst);
            if (!first && ci->second != inst) ci->second = kAmbiguous;
        }
        // The member of a record this call builds, whatever object; the
        // call is visited once per instance of its function.
        if (std::optional<AbstractQueue> m = memberOf(call.getOperand(0), inst)) {
            auto &calls = memberCtors[{m->record, m->offset}];
            if (!llvm::is_contained(calls, op)) calls.push_back(op);
        }
    }

    struct Resolved {
        Operation *op;
        Instance *inst;
        llvm::SmallVector<Key, 4> rs;
    };
    llvm::SmallVector<Resolved, 32> resolved;
    llvm::SmallSetVector<Operation *, 4> abstractUsed;   // constructor calls some access fell back to
    // The places each access may name, once: an array is counted as one
    // queue only when some access or constructor reaches it with an index
    // the walk cannot read, or one constructor call builds every element;
    // read only with constant indices, and built element by element, each
    // element stays a queue of its own.
    // A free whose pool the analysis cannot name puts into some pool that
    // may hold mbufs; it is counted on each of them once all are known.
    llvm::erase_if(visits, [&](const std::pair<Operation *, Instance *> &v) {
        if (libraryOp(v.first).viaMbuf == MbufFree::None) return false;
        std::string why;
        if (mbufPool(v.first, &why)) return false;
        unresolvedPuts.push_back({v.first, v.second, why});
        return true;
    });
    llvm::SmallVector<llvm::SmallVector<Place, 4>, 32> placesOf;
    placesOf.reserve(visits.size());
    for (auto [op, inst] : visits) {
        placesOf.push_back(walk.resolvePlaces(queueOf(op), inst));
        for (const Place &pl : placesOf.back())
            if (pl.root && (pl.unknownIndex || pl.loop) &&
                elementsHold({pl.root, kElementsOffset}, queueObjectTypeOfOp(op)))
                elementKeys.insert({pl.root, kElementsOffset});
    }
    for (auto [vi, visit] : llvm::enumerate(visits)) {
        Operation *op = visit.first;
        Instance *inst = visit.second;
        // A handle may name one of several queues -- loaded from a field or
        // a slot written in more than one place -- and then the access is
        // counted on each of them: a thread too many on a queue it does not
        // touch costs a specialisation, a thread missing from one it does
        // would let a race through.
        llvm::SmallVector<Key, 4> rs;
        const char *why = nullptr;
        const Type queueTy = queueObjectTypeOfOp(op);
        // A place the walk could not name, or named without a queue there,
        // may still be a member of a record reached through a pointer the
        // walk cannot follow: then it is that member of some object of the
        // record (AbstractQueue), which stands for all of them.
        auto unidentified = [&]() {
            if (std::optional<Key> a = abstractKey(op, inst)) {
                if (!llvm::is_contained(rs, *a)) rs.push_back(*a);
                abstractUsed.insert(a->first);
                return;
            }
            why = "the queue object could not be identified";
        };
        for (const Place &pl : placesOf[vi]) {
            if (!pl.root) { unidentified(); if (why) break; continue; }
            // An element of an array of queues: every element, counted
            // together, whichever index this access uses -- `&qs[2]` is one
            // of the elements the others may name.
            const Key ek{pl.root, kElementsOffset};
            if (elementKeys.contains(ek) && elementsHold(ek, queueTy)) {
                if (!llvm::is_contained(rs, ek)) rs.push_back(ek);
                continue;
            }
            if (pl.unknownIndex || pl.loop) { why = "the queue is an element of an array of unknown queues"; break; }
            // A queue is the whole of a global or a slot, or what some
            // constructor call builds; anything else at that place -- a
            // struct whose first member is not the queue, a heap object
            // whose constructor the walk never saw -- is not one.
            const Key k{pl.root, pl.offset};
            if (!isWhole(k, queueTy) && keyType.lookup(k) != queueTy) {
                unidentified();
                if (why) break;
                continue;
            }
            rs.push_back(k);
        }
        if (why) { poison(op, why); continue; }
        resolved.push_back({op, inst, std::move(rs)});
    }

    // An array whose elements are counted together takes along every place
    // in it the walk did name: a constructor of `qs[2]` builds one of the
    // elements the others stand for.
    if (!elementKeys.empty()) {
        // Only a queue of the elements' type: another member of the same
        // root -- `struct { queue<int> in; queue<item> st[3]; }` -- is a
        // queue of its own. Decided once, before anything moves, so that
        // the constructors and the accesses of a key go to the same place.
        llvm::DenseMap<Key, Key> moveTo;
        auto decide = [&](const Key &k) {
            if (k.second == kElementsOffset || k.second == kAbstractOffset || moveTo.count(k)) return;
            const Key ek{k.first, kElementsOffset};
            if (!elementKeys.contains(ek)) return;
            const Type t = queueTypeOf(k), et = queueTypeOf(ek);
            if (t && t == et) moveTo[k] = ek;
        };
        for (auto &kv : keyCtors) decide(kv.first);
        for (auto &kv : keyType) decide(kv.first);
        for (const Resolved &r : resolved)
            for (const Key &k : r.rs) decide(k);
        auto into = [&](const Key &k) -> Key {
            auto it = moveTo.find(k);
            return it == moveTo.end() ? k : it->second;
        };
        for (auto &[ctor, ks] : ctorKeys) {
            llvm::SmallSetVector<Key, 2> moved;
            for (const Key &k : ks) moved.insert(into(k));
            ks = std::move(moved);
        }
        for (auto &[k, ek] : moveTo) {
            if (auto it = keyCtors.find(k); it != keyCtors.end()) {
                llvm::SmallVector<Operation *, 2> cs(it->second.begin(), it->second.end());
                keyCtors.erase(k);
                for (Operation *c : cs) keyCtors[ek].insert(c);
            }
            keyType.erase(k);   // of the elements' type, by the choice above
            if (const Instance *ci = keyCtorInst.lookup(k)) {
                auto [it, fresh] = keyCtorInst.try_emplace(ek, ci);
                if (!fresh && it->second != ci) it->second = kAmbiguous;
            }
            keyCtorInst.erase(k);
        }
        for (Resolved &r : resolved) {
            llvm::SmallVector<Key, 4> moved;
            for (const Key &k : r.rs) {
                const Key c = into(k);
                if (!llvm::is_contained(moved, c)) moved.push_back(c);
            }
            r.rs = std::move(moved);
        }
    }

    // Every object a constructor call builds is among those its abstract
    // queue stands for: once that queue is in use, the objects the walk did
    // place under the same constructor are folded into it, or the one
    // constructor would carry two verdicts.
    llvm::DenseMap<Key, Key> folded;
    for (Operation *ctor : abstractUsed) {
        const Key a{ctor, kAbstractOffset};
        for (const Key &k : ctorKeys[ctor])
            if (k != a) folded[k] = a;
        ctorKeys[ctor].clear();
        ctorKeys[ctor].insert(a);
    }
    for (auto &[k, a] : folded) {
        keyCtors.erase(k);
        keyType.erase(k);
    }

    for (Resolved &r : resolved) {
        Operation *op = r.op;
        Instance *inst = r.inst;
        llvm::SmallVector<Key, 4> rs;
        for (const Key &k : r.rs) {
            auto it = folded.find(k);
            const Key c = it == folded.end() ? k : it->second;
            if (!llvm::is_contained(rs, c)) rs.push_back(c);
        }
        for (const Key &root : rs) {
            RootInfo &info = roots[root];
            if (LibraryQueueCall l = libraryOp(op)) {
                libraryKeys.insert(root);
                // A pool the walk places but whose creation is not in
                // sight: it may hold mbufs.
                if (l.kind == LibraryQueueKind::Mempool && !isLibraryCreate(root.first)) mbufPoolKeys.insert(root);
            }
            // An overrunning push (shim/bounded.hpp) takes the oldest item
            // out when the queue is full: its thread is on both sides.
            auto push = dyn_cast<QueuePushOp>(op);
            if (push && push.getOverrun()) {
                SideInfo &also = info.consumers;
                also.threads.insert(inst->thread);
                Access b = accessOf(op, *inst);
                b.oneOf = rs.size() > 1;
                also.accesses.push_back(b);
                if (inst->threadMult == Mult::Many) also.anyMany = true;
            }
            SideInfo &side = isConsumerOp(op) ? info.consumers : info.producers;
            side.threads.insert(inst->thread);
            Access a = accessOf(op, *inst);
            a.oneOf = rs.size() > 1;
            // Credit the thread to its loop only when that run of the loop
            // belongs to the constructor of the object it touches: for an
            // abstract queue, the captured `this` is that run's; for a
            // placed one, the run is the one that built the queue, or the
            // loop is not in the constructor's function at all, where no
            // choice is made at construction.
            if (Operation *loop = inst->countLoop) {
                const Instance *holder = inst->countLoopInst;
                bool credited = false;
                if (abstractKeys.count(root)) {
                    credited = sameObject(op, inst, holder);
                } else {
                    auto loopFn = loop->getParentOfType<cir::FuncOp>();
                    bool ctorInLoopFn = false;
                    if (auto kc = keyCtors.find(root); kc != keyCtors.end())
                        for (Operation *c : kc->second)
                            if (c->getParentOfType<cir::FuncOp>() == loopFn) ctorInLoopFn = true;
                    const Instance *built = keyCtorInst.lookup(root);
                    credited = !ctorInLoopFn || (built && built != kAmbiguous && built == holder);
                }
                if (credited) a.countLoop = loop;
            }
            side.accesses.push_back(a);
            if (inst->threadMult == Mult::Many) side.anyMany = true;
            opRoots[op].insert(root);
        }
    }

    addPoolsInitialisedLater(objectPools.getArrayRef());
    // Each unresolved free is a put, on its thread, into one of the pools
    // that may hold mbufs: counted on every one, as one of several, so it
    // can make a side many but never proves it.
    for (const UnresolvedPut &u : unresolvedPuts)
        for (const Key &k : mbufPoolKeys) {
            SideInfo &side = roots[k].producers;
            libraryKeys.insert(k);
            side.threads.insert(u.inst->thread);
            Access a = accessOf(u.op, *u.inst);
            a.oneOf = true;
            a.poolWhy = u.why;
            side.accesses.push_back(a);
            if (u.inst->threadMult == Mult::Many) side.anyMany = true;
        }

    remarks += walk.remarks();

    // Two different failures, and telling them apart is the difference
    // between "this program is beyond me" and "this program is too big",
    // which call for different work. One message hid that.
    if (walk.capExceeded()) {
        module.emitRemark() << "wtc: gave up after " << ThreadInstanceWalk::kInstanceCap
                            << " function instances; every queue is unknown";
        ++remarks;
    }
    // Before the remarks below: it may leave an access with no type.
    poisonAddressTakenLibraryOps();
    poisonUnmodelledMempoolCalls();
    poisonEscapes();
    if (globalPoison) {
        module.emitRemark() << "wtc: an unresolvable queue access had no type to attribute it to; "
                               "every queue is unknown";
        ++remarks;
    }
    writeVerdicts(walk.capExceeded());
    markAllAnalysesPreserved();
}

} // namespace

namespace mlir::wtc {
    void registerQueueOwnershipAnalysisPass() {
        PassRegistration<QueueOwnershipAnalysisPass>();
    }
}
