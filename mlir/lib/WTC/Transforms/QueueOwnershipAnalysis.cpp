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
#include "llvm/ADT/SmallSet.h"
#include "llvm/Demangle/Demangle.h"
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
    // The counted loop that starts every thread on this side, one per
    // iteration, when there is one: the side then has one thread exactly
    // when the loop runs at most once. The walk may tell several threads
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
        return op->getOperand(libraryOp(op).queueOperand);
    }

    static Type queueObjectType(Operation *root) {
        if (auto g = dyn_cast<cir::GlobalOp>(root)) return g.getSymType();
        if (auto a = dyn_cast<cir::AllocaOp>(root)) return a.getAllocaType();
        return Type();
    }

    Type queueObjectTypeOfOp(Operation *op) {
        Value q = queueOf(op);
        if (auto qt = dyn_cast<QueueType>(q.getType())) return qt.getElementType();
        if (auto pt = dyn_cast<cir::PointerType>(q.getType())) return pt.getPointee();
        return Type();
    }

    // A call of the shim's queue constructor, or of a modelled library
    // queue's, whose first operand is the object it builds; or
    // rte_ring_create, whose result is.
    bool isQueueConstructor(Operation *op) {
        auto call = dyn_cast<cir::CallOp>(op);
        if (!call) return false;
        if (getSemanticKind(lookupCallee(call)) == SemanticOpKind::QueueConstruction)
            return call.getNumOperands() > 0 && firstRealOperand(call) == 0;
        LibraryQueueCall l = libraryCall(call);
        return l.role == LibraryQueueRole::Create ||
               (l.role == LibraryQueueRole::Construct && l.queueOperand == 0);
    }
    bool isRingCreate(Operation *op) {
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

    void poisonReachable(cir::FuncOp f) {
        llvm::DenseSet<Operation *> seen;
        llvm::SmallVector<cir::FuncOp, 8> work{f};
        while (!work.empty()) {
            cir::FuncOp cur = work.pop_back_val();
            if (!seen.insert(cur.getOperation()).second) continue;
            cur.walk([&](Operation *op) {
                if (isQueueOp(op) && !inLibraryBody(op)) poison(op, "reached through recursion");
                if (auto call = dyn_cast<cir::CallOp>(op))
                    if (auto callee = lookupCallee(call))
                        if (!callee.getBody().empty()) work.push_back(callee);
            });
        }
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

// Whether a function's first argument is the hidden return slot.
static bool firstArgIsSret(cir::FuncOp f) {
    auto attrs = f.getArgAttrsAttr();
    if (!attrs || attrs.empty()) return false;
    auto d = dyn_cast<DictionaryAttr>(attrs[0]);
    return d && d.get("llvm.sret");
}

// What a modelled library queue declares. A class declares one thread on
// each side through its type, whether its constructor is in sight or only
// its pushes and pops are. An rte_ring declares a side single through an
// rte_ring_sp_* or rte_ring_sc_* call on that side, and through its flags
// (RING_F_SP_ENQ 0x1, RING_F_SC_DEQ 0x2, rte_ring_core.h) only when some
// call on that side follows them: a side reached only by rte_ring_mp_* or
// rte_ring_mc_* calls is many whatever the flags say. Flags computed at run
// time, given differently by two creations, or never seen because the
// creation is out of sight leave such a side unknown.
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
    std::optional<int64_t> flags;
    if (ctors.empty()) d.unknown = "its creation is not in this translation unit, so its flags are not known";
    for (cir::CallOp ctor : ctors) {
        LibraryQueueCall l = libraryCall(ctor);
        std::optional<int64_t> f = l.flagsOperand && *l.flagsOperand < ctor.getNumOperands()
                                       ? constantInteger(ctor.getOperand(*l.flagsOperand))
                                       : std::nullopt;
        if (!f) {
            d.unknown = "its flags are computed at run time";
            break;
        }
        if (flags && (*flags & 3) != (*f & 3)) {
            d.unknown = "its creations give it different flags";
            break;
        }
        flags = f;
    }
    for (auto [side, out, bit] : {std::tuple{&info.producers, &d.producers, int64_t(1)},
                                  std::tuple{&info.consumers, &d.consumers, int64_t(2)}}) {
        const Access *single = nullptr;
        bool followsFlags = false;
        for (const Access &a : side->accesses) {
            const LibraryQueueSync sync = syncOf(a.op);
            if (sync == LibraryQueueSync::Single && !single) single = &a;
            if (sync == LibraryQueueSync::ByFlags) followsFlags = true;
        }
        if (single) {
            *out = Declared::One;
            auto call = cast<cir::CallOp>(single->op);
            if (!d.how.empty()) d.how += ", and ";
            d.how += ("by " + lookupCallee(call).getSymName() + " at " +
                      whereOf(call.getLoc(), /*fileOnly=*/true)).str();
        } else if (followsFlags) {
            if (!d.unknown.empty()) *out = Declared::Unknown;
            else if (*flags & bit) {
                *out = Declared::One;
                if (d.how.find("by its flags") == std::string::npos)
                    d.how = d.how.empty() ? "by its flags" : "by its flags, and " + d.how;
            }
        }
    }
    return d;
}

std::string QueueOwnershipAnalysisPass::openWhy(Type t) {
    auto [it, fresh] = openWhyCache.try_emplace(t);
    if (!fresh) return it->second;
    std::string why;
    auto isRing = [](Type ty) {
        auto rec = dyn_cast_or_null<cir::RecordType>(ty);
        return rec && rec.getName() && rec.getName().getValue() == "rte_ring";
    };
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
    module.walk([&](cir::CallOp call) {
        if (!why.empty()) return WalkResult::interrupt();
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return WalkResult::advance();
        if (auto f = call->getParentOfType<cir::FuncOp>(); f && isLibraryFunction(f))
            return WalkResult::advance();
        if (isRing(t) && callee.getSymName() == "rte_ring_lookup") {
            why = "rte_ring_lookup hands rings to code that finds them by name";
            return WalkResult::interrupt();
        }
        // A thread start's argument goes to an entry the walk follows.
        if (!callee.getBody().empty() || isLibraryFunction(callee) || isAllocator(callee) ||
            touchesOnlyItsArguments(callee) || callee.getSymName() == "free" ||
            getSemanticKind(callee) == SemanticOpKind::ThreadSpawn)
            return WalkResult::advance();
        if (llvm::any_of(call.getOperands(), reaches))
            why = ("it is handed to " + callee.getSymName() + ", whose body is not here").str();
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
        const std::string why = "the address of " + llvm::demangle(f.getSymName().str()) +
                                " is taken, and calls through it are not counted";
        if (!pt) globalPoison = true;
        else if (poisonedTypes.insert(pt.getPointee()).second) poisonWhy[pt.getPointee()] = why;
        taken->emitRemark() << "wtc: " << why << "; every queue of its type is now unknown";
        ++remarks;
    });
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
        if (*tag == 0 && poisoned != poisonWhy.end()) {
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
        InFlightDiagnostic r =
            at->emitRemark() << "wtc: queue " << label << ": declared " << declared << ", verified";
        // A library queue may be shared with code the module does not show:
        // another file, another process, a library that takes it whole or
        // inside a structure. The verdict is about this one.
        if (library) r << " in this translation unit" << (open.empty() ? "" : "; " + open);
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
    else if (llvm::any_of(info.producers.accesses, [](const Access &a) { return a.oneOf; }) ||
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
    libraryFunctions.clear();
    globalPoison = false; remarks = 0;
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
        if (isRingCreate(op)) {
            auto made = call.getNumResults() ? dyn_cast<cir::PointerType>(call.getResult().getType())
                                             : cir::PointerType();
            if (!made) { ctorUnplaced.insert(op); continue; }
            const Key k{op, 0};
            ctorKeys[op].insert(k);
            keyCtors[k].insert(op);
            keyType.try_emplace(k, made.getPointee());
            auto [ci, first] = keyCtorInst.try_emplace(k, inst);
            if (!first && ci->second != inst) ci->second = kAmbiguous;
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
            if (libraryOp(op)) libraryKeys.insert(root);
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
