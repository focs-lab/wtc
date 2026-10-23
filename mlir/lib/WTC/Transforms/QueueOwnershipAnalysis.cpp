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
};

struct RootInfo {
    SideInfo producers, consumers;
};

// A queue: the root the walk names, and the queue's offset in it.
using Key = std::pair<Operation *, uint64_t>;

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
    llvm::DenseMap<Type, std::string> poisonWhy;   // the first reason, for the declared-topology check
    llvm::DenseMap<Operation *, llvm::SmallSetVector<Key, 2>> opRoots;
    // Which queues each constructor call builds, under every instance of
    // its function, and the reverse; a call whose object the walk cannot
    // name builds something unknown and is no queue's own.
    llvm::DenseMap<Operation *, llvm::SmallSetVector<Key, 2>> ctorKeys;
    llvm::DenseMap<Key, llvm::SmallSetVector<Operation *, 2>> keyCtors;
    llvm::DenseSet<Operation *> ctorUnplaced;
    llvm::DenseMap<Key, Type> keyType;             // the queue type its constructor builds
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

    void poison(Operation *op, const Twine &why) {
        Type t = queueObjectTypeOfOp(op);
        if (t && poisonedTypes.insert(t).second) poisonWhy[t] = why.str();
        else globalPoison = true;
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
                  static_cast<bool>(site)};
}

Operation *QueueOwnershipAnalysisPass::carrierOf(const Key &k, const char *&why) const {
    auto it = keyCtors.find(k);
    if (it == keyCtors.end() || it->second.empty()) { why = "no constructor call is seen to build it"; return nullptr; }
    if (it->second.size() != 1) { why = "more than one constructor call may build it"; return nullptr; }
    Operation *c = it->second.front();
    if (ctorUnplaced.contains(c)) { why = "its constructor call also builds an object the analysis cannot name"; return nullptr; }
    if (ctorKeys.lookup(c).size() != 1) { why = "its constructor call also builds other queues"; return nullptr; }
    return c;
}

std::string QueueOwnershipAnalysisPass::labelOf(const Key &k, bool whole) const {
    Operation *root = k.first;
    std::string base;
    if (auto g = dyn_cast<cir::GlobalOp>(root)) base = ("@" + g.getSymName()).str();
    else if (auto a = dyn_cast<cir::AllocaOp>(root)) base = ("%" + a.getName()).str();
    else base = "the heap object made at " + whereOf(root->getLoc(), /*fileOnly=*/true);
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
    const char *declared = *tag == 2 ? "spsc_inline" : "spsc";
    if (verdict == Topology::spsc) {
        ctor.emitRemark() << "wtc: queue " << label << ": declared " << declared << ", verified";
        ++remarks;
        return true;
    }
    // Certainly broken: on one side, two threads the module accounts for,
    // or one that certainly stands for several.
    for (const SideInfo *side : {&info.producers, &info.consumers}) {
        if (summary) break;
        const bool pushes = side == &info.producers;
        llvm::SmallVector<const Access *, 4> firstOfThread;
        llvm::SmallSet<unsigned, 4> seen;
        bool certainMany = false;
        for (const Access &a : side->accesses) {
            if (a.speculative || a.oneOf) continue;
            if (a.many) certainMany = true;
            if (seen.insert(a.thread).second) firstOfThread.push_back(&a);
        }
        if (firstOfThread.size() < 2 && !certainMany) continue;
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
    // Not spsc, but nothing certain: say why it could not be checked.
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
        // An operation that may act on several queues names none of them:
        // a later pass reading queue_id would otherwise treat the others'
        // operations as unrelated to this one.
        if (ks.size() != 1) continue;
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
    globalPoison = false; remarks = 0;

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
        }
    }

    for (auto [op, inst] : visits) {
        // A handle may name one of several queues -- loaded from a field or
        // a slot written in more than one place -- and then the access is
        // counted on each of them: a thread too many on a queue it does not
        // touch costs a specialisation, a thread missing from one it does
        // would let a race through.
        llvm::SmallVector<Key, 4> rs;
        const char *why = nullptr;
        const Type queueTy = queueObjectTypeOfOp(op);
        for (const Place &pl : walk.resolvePlaces(queueOf(op), inst)) {
            if (!pl.root) { why = "the queue object could not be identified"; break; }
            // One element of an array of queues: which one is the thread's,
            // and every element is built by one constructor call.
            if (pl.unknownIndex || pl.loop) { why = "the queue is an element of an array"; break; }
            // A queue is the whole of a global or a slot, or what some
            // constructor call builds; anything else at that place -- a
            // struct whose first member is not the queue, a heap object
            // whose constructor the walk never saw -- is not one.
            const Key k{pl.root, pl.offset};
            if (!isWhole(k, queueTy) && keyType.lookup(k) != queueTy) {
                why = "the queue object could not be identified";
                break;
            }
            rs.push_back(k);
        }
        if (why) { poison(op, why); continue; }
        for (const Key &root : rs) {
            RootInfo &info = roots[root];
            SideInfo &side = isa<QueuePopOp>(op) ? info.consumers : info.producers;
            side.threads.insert(inst->thread);
            side.accesses.push_back(accessOf(op, *inst));
            side.accesses.back().oneOf = rs.size() > 1;
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
