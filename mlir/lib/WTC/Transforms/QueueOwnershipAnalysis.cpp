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

    // Every operation on a queue. Only a pop consumes; push and the three
    // operations of a batched run are the producer's side.
    static bool isQueueOp(Operation *op) {
        return isa<QueuePushOp, QueuePopOp, QueueBatchBeginOp, QueuePushDeferredOp, QueueBatchEndOp>(op);
    }
    static Value queueOf(Operation *op) {
        if (auto o = dyn_cast<QueuePushOp>(op)) return o.getQueue();
        if (auto o = dyn_cast<QueuePopOp>(op)) return o.getQueue();
        if (auto o = dyn_cast<QueueBatchBeginOp>(op)) return o.getQueue();
        if (auto o = dyn_cast<QueuePushDeferredOp>(op)) return o.getQueue();
        return cast<QueueBatchEndOp>(op).getQueue();
    }

    static Type queueObjectType(Operation *root) {
        if (auto g = dyn_cast<cir::GlobalOp>(root)) return g.getSymType();
        if (auto a = dyn_cast<cir::AllocaOp>(root)) return a.getAllocaType();
        return Type();
    }

    static Type queueObjectTypeOfOp(Operation *op) {
        Value q = queueOf(op);
        if (auto qt = dyn_cast<QueueType>(q.getType())) return qt.getElementType();
        return Type();
    }

    // A call of the shim's queue constructor, whose first operand is the
    // object it builds.
    static bool isQueueConstructor(Operation *op) {
        auto call = dyn_cast<cir::CallOp>(op);
        return call && getSemanticKind(lookupCallee(call)) == SemanticOpKind::QueueConstruction &&
               call.getNumOperands() > 0 && firstRealOperand(call) == 0;
    }
    static bool isQueueOpOrConstructor(Operation *op) { return isQueueOp(op) || isQueueConstructor(op); }

    // The whole of a global or a slot of the queue's own type.
    static bool isWhole(const Key &k, Type queueTy) {
        return k.second == 0 && isa<cir::GlobalOp, cir::AllocaOp>(k.first) && queueObjectType(k.first) == queueTy;
    }
    // The queue type at a key: what its constructor builds, or the root's
    // own type for the whole of a global or a slot.
    Type queueTypeOf(const Key &k) const {
        if (Type t = keyType.lookup(k)) return t;
        return k.second == 0 ? queueObjectType(k.first) : Type();
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
                if (isQueueOp(op)) poison(op, "reached through recursion");
                if (auto call = dyn_cast<cir::CallOp>(op))
                    if (auto callee = lookupCallee(call))
                        if (!callee.getBody().empty()) work.push_back(callee);
            });
        }
    }

    // A queue's number is its position in discovery order, which follows
    // the walk and is therefore deterministic.
    int64_t idOf(const Key &k) const {
        return static_cast<int64_t>(roots.find(k) - roots.begin());
    }

    void writeVerdicts(bool capExceeded);
    // False when a declared topology is certainly broken. A summary queue
    // stands for every object one allocation site makes, and threads on two
    // of them are no proof against either.
    bool checkDeclared(Operation *root, const RootInfo &info, Topology verdict, const std::string &label,
                       llvm::ArrayRef<cir::CallOp> ctors, bool capExceeded, bool summary);
    static Access accessOf(Operation *op, const Instance &inst);
    // Why one side of a queue counts many, in the program's own terms.
    std::string whyMany(const SideInfo &side);
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
            const bool spawn = getSemanticKind(lookupCallee(site)) == SemanticOpKind::ThreadSpawn;
            return std::string(spawn ? "started" : "reached by a call") + " in a loop at " +
                   whereOf(site.getLoc(), /*fileOnly=*/true);
        }
    }
    return "";
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

bool QueueOwnershipAnalysisPass::checkDeclared(Operation *root, const RootInfo &info, Topology verdict,
                                               const std::string &label,
                                               llvm::ArrayRef<cir::CallOp> ctors, bool capExceeded,
                                               bool summary) {
    auto tag = constructorTag(ctors);
    if (!tag || *tag == 0) return true;  // nothing declared, or nothing readable
    cir::CallOp ctor = ctors.front();
    // mpsc promises one consumer and any number of producers, spmc one
    // producer and any number of consumers; the others, one of each.
    const bool declaredMpsc = *tag == 3, declaredSpmc = *tag == 4;
    const char *declared = *tag == 2       ? "spsc_inline"
                           : declaredMpsc ? "mpsc"
                           : declaredSpmc ? "spmc"
                                          : "spsc";
    if (verdict == Topology::spsc || (declaredMpsc && verdict == Topology::mpsc) ||
        (declaredSpmc && verdict == Topology::spmc)) {
        ctor.emitRemark() << "wtc: queue " << label << ": declared " << declared << ", verified";
        ++remarks;
        return true;
    }
    // Certainly broken: on one side, two threads the module accounts for
    // that can both exist, or one that certainly stands for several.
    const char *exclusiveSide = nullptr;
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
        bool coexist = false;
        for (size_t i = 0; i < firstOfThread.size() && !coexist; ++i)
            for (size_t j = i + 1; j < firstOfThread.size() && !coexist; ++j)
                coexist = !walk->exclusive(firstOfThread[i]->thread, firstOfThread[j]->thread);
        if (!certainMany && !coexist) {
            if (firstOfThread.size() >= 2) exclusiveSide = pushes ? "push to it" : "pop from it";
            continue;
        }
        InFlightDiagnostic d = ctor.emitError()
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
    InFlightDiagnostic w = ctor.emitWarning()
                           << "wtc: queue " << label << " is declared " << declared << "; cannot verify: ";
    auto built = dyn_cast<cir::PointerType>(ctor.getOperand(0).getType());
    const Type objTy = built ? built.getPointee() : queueObjectType(root);
    if (capExceeded) w << "the program has too many function instances";
    else if (globalPoison) w << "an access to some queue could not be attributed";
    else if (auto it = poisonWhy.find(objTy); it != poisonWhy.end()) w << it->second;
    else if (summary) w << "it is one of the objects one allocation site makes, and the threads may each have their own";
    else if (llvm::any_of(info.producers.accesses, [](const Access &a) { return a.oneOf; }) ||
             llvm::any_of(info.consumers.accesses, [](const Access &a) { return a.oneOf; }))
        w << "an operation may act on it or on another queue";
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
        const unsigned p = info.producers.count(), c = info.consumers.count();
        const Type objTy = queueTypeOf(key);
        if (globalPoison || capExceeded || (objTy && poisonedTypes.count(objTy)))
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
        const std::string label = labelOf(key, whole);
        const char *noCarrier = nullptr;
        Operation *carrier = whole ? root : carrierOf(key, noCarrier);
        if (carrier) {
            carrier->setAttr(kTopologyAttrName, TopologyAttr::get(ctx, t));
            carrier->setAttr(kQueueIdAttrName, IntegerAttr::get(IntegerType::get(ctx, 64), idOf(key)));
            if (!whole) carrier->setAttr(kQueueNameAttrName, StringAttr::get(ctx, label));
        }
        root->emitRemark() << "wtc: queue " << label << ": " << stringifyTopology(t)
                           << " (producers: " << (p == 2 ? "many" : std::to_string(p))
                           << ", consumers: " << (c == 2 ? "many" : std::to_string(c)) << ")";
        ++remarks;
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
        if (!carrier && t == Topology::spsc) {
            root->emitRemark() << "wtc: queue " << label << " keeps the general-purpose implementation: "
                               << noCarrier;
            ++remarks;
        }
        llvm::SmallVector<cir::CallOp, 2> ctors;
        if (whole) {
            if (auto it = ctorsOf.find(root); it != ctorsOf.end()) ctors.assign(it->second.begin(), it->second.end());
        } else if (auto it = keyCtors.find(key); it != keyCtors.end()) {
            for (Operation *c : it->second) ctors.push_back(cast<cir::CallOp>(c));
        }
        if (!ctors.empty()) {
            const bool summary = isa<cir::CallOp>(root) && !walk->siteRunsOnce(root);
            contractsHold &= checkDeclared(root, info, t, label, ctors, capExceeded, summary);
        }
    }
    if (!contractsHold) signalPassFailure();

    // Each operation gets the meet over every queue it may act on.
    for (auto &[op, ks] : opRoots) {
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
        if (isQueueOp(op) && !op->getAttr("topology"))
            op->setAttr("topology", TopologyAttr::get(ctx, Topology::unknown));
    });
}

void QueueOwnershipAnalysisPass::runOnOperation() {
    module = getOperation();
    roots.clear(); poisonedTypes.clear(); poisonWhy.clear(); opRoots.clear();
    ctorKeys.clear(); keyCtors.clear(); ctorUnplaced.clear(); keyType.clear();
    memberCtors.clear(); abstractKeys.clear(); layout.reset();
    keyCtorInst.clear(); captureStores.clear();
    globalPoison = false; remarks = 0;
    poisonedOps.clear();

    // A function that can reach neither a queue operation nor a queue's
    // constructor cannot affect a verdict, nor which call builds a queue.
    // The predicate needs the walk that owns the memo table, and the walk
    // needs the predicate, so the lambda is named: a temporary would die at
    // the end of the constructor call and leave function_ref dangling.
    ThreadInstanceWalk *self = nullptr;
    auto relevant = [&](cir::FuncOp f) { return self->reachesOp(f, isQueueOpOrConstructor); };
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
        auto built = dyn_cast<cir::PointerType>(call.getOperand(0).getType());
        for (const Place &pl : walk.resolvePlaces(call.getOperand(0), inst)) {
            if (!built || !pl.root || pl.unknownIndex || pl.loop) { ctorUnplaced.insert(op); continue; }
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
    for (auto [op, inst] : visits) {
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
        for (const Place &pl : walk.resolvePlaces(queueOf(op), inst)) {
            if (!pl.root) { unidentified(); if (why) break; continue; }
            // One element of an array of queues: which one is the thread's,
            // and every element is built by one constructor call.
            if (pl.unknownIndex || pl.loop) { why = "the queue is an element of an array"; break; }
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
            SideInfo &side = isa<QueuePopOp>(op) ? info.consumers : info.producers;
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
