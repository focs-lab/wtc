#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/ThreadInstances.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCOps.h"

using namespace mlir;
using namespace mlir::wtc;

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "clang/CIR/Dialect/IR/CIRTypes.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallSet.h"

#include <numeric>
#include <string>

// Two fields of one object, written by two different threads, landing in one
// cache line. Nothing in the program says so and nothing reports it: the
// line simply travels between the cores for the rest of the run. How much
// that costs is a measurement, made per program under the protocol in
// docs/gate.md, and no figure is quoted here: an earlier version quoted
// one from an unreserved probe, which a review found recorded nowhere.
//
// Tools find this by running the program: perf c2c samples the coherence
// traffic, and the research detectors instrument and watch. All of them see
// only what that execution touched, report an address rather than a field,
// and slow the program down while they look. The information needed to
// answer it statically is which threads touch which field, which is exactly
// what the ownership walk already computes.
//
// This pass only reports. It changes no byte of the program, so it carries
// none of the layout risk. Its output is a list of pairs that *may* be worth
// separating, not a list that should be: a verdict says which threads touch
// which fields and how the fields lie, and nothing about how often. On
// memcached every pair it reported turned out to be serialised by a lock or
// written too rarely to matter (docs/corpus.md), so a reader checks a pair
// against the source before acting on it.
//
// What it will not claim. A thread that stands for many dynamic threads
// (spawned in a loop, or of unknown provenance) touching two fields is
// reported separately as possible rather than definite, because the analysis
// cannot tell whether one such thread touched both fields or two did. An
// object it cannot resolve to a root is counted and named, not guessed at.

namespace {

// Where an access lands: a root, and what its offset is relative to.
// Absolute: the root's start. Relative: the start of the element a spawn
// loop handed the thread (`&arr[i]`, i the loop's induction variable), so
// which element depends on the thread. Unknown: the start of some element
// of the root, at an index the analysis cannot read.
enum class PlaceKind { Absolute, Relative, Unknown };
struct PlaceKey {
    Operation *root = nullptr;
    PlaceKind kind = PlaceKind::Absolute;
    bool operator==(const PlaceKey &o) const { return root == o.root && kind == o.kind; }
};

} // namespace

namespace llvm {
template <> struct DenseMapInfo<PlaceKey> {
    // No operation lives at these addresses.
    static PlaceKey getEmptyKey() { return {reinterpret_cast<Operation *>(uintptr_t(-1)), PlaceKind::Absolute}; }
    static PlaceKey getTombstoneKey() { return {reinterpret_cast<Operation *>(uintptr_t(-2)), PlaceKind::Absolute}; }
    static unsigned getHashValue(const PlaceKey &k) {
        return static_cast<unsigned>(hash_combine(k.root, static_cast<int>(k.kind)));
    }
    static bool isEqual(const PlaceKey &a, const PlaceKey &b) { return a == b; }
};
} // namespace llvm

namespace {

struct FieldInfo {
    uint64_t size = 0;
    std::string name;
    llvm::SmallSetVector<unsigned, 4> readers, writers;
    // Threads whose access repeats: the site is in a loop, or the function
    // runs many times, or the thread stands for many. A line only travels
    // for as long as both sides keep touching it, so a pair where either
    // side touches once is a single invalidation and costs nothing.
    llvm::SmallSetVector<unsigned, 4> hotReaders, hotWriters;
    // Threads whose access rate the walk cannot tell: the function's
    // control flow is unstructured (a goto), so "inside a loop" has no
    // meaning there. Neither once nor repeated; such a side can make a
    // pair possible and never definite or cold.
    llvm::SmallSetVector<unsigned, 4> unknownRateReaders, unknownRateWriters;
    // The widest repeated access, and the widest once-only access, each
    // thread makes at this offset. A pair is judged on the two accesses
    // that make it, not on the widest access anyone made: a start-up
    // snapshot of a whole record once hid every pair of its members,
    // because the record's size was taken for the member's -- and when the
    // snapshot was the writer's own, one size per thread was not enough.
    llvm::DenseMap<unsigned, uint64_t> hotSize, onceSize;
    // Each thread's first write, so a report about a once-writer can point
    // at that thread's own store.
    llvm::DenseMap<unsigned, Operation *> writeAt;
    // Threads this module can account for: descended from main, from a spawn
    // it can see, or from a function whose address it takes. A thread
    // invented for an externally visible function nobody here references is
    // a placeholder for "some caller somewhere", and two placeholders are
    // not two threads.
    llvm::SmallSetVector<unsigned, 4> certain;
    // Where each contending thread touches the field, so a report can name
    // the two accesses that make the verdict rather than the first write it
    // happened to see. A reader cannot check a finding without them.
    llvm::DenseMap<unsigned, Operation *> hotWriteAt, hotReadAt;
    // Hot writers whose thread stands for many: two of those threads may be
    // on two fields of one line, and the analysis cannot tell.
    llvm::SmallSetVector<unsigned, 4> manyHotWriters;
    Operation *firstWrite = nullptr;
    Operation *firstAccess = nullptr;

    // Takes another field's accesses as this field's: used when an access
    // at a constant index is folded into the element it falls in.
    void absorb(const FieldInfo &o) {
        size = std::max(size, o.size);
        if (name.empty() || name == "<whole>" || (o.name != "<whole>" && o.name.size() > name.size()))
            name = o.name;
        for (unsigned t : o.readers) readers.insert(t);
        for (unsigned t : o.writers) writers.insert(t);
        for (unsigned t : o.hotReaders) hotReaders.insert(t);
        for (unsigned t : o.hotWriters) hotWriters.insert(t);
        for (unsigned t : o.unknownRateReaders) unknownRateReaders.insert(t);
        for (unsigned t : o.unknownRateWriters) unknownRateWriters.insert(t);
        for (unsigned t : o.certain) certain.insert(t);
        for (unsigned t : o.manyHotWriters) manyHotWriters.insert(t);
        for (auto &kv : o.hotSize) { uint64_t &m = hotSize[kv.first]; m = std::max(m, kv.second); }
        for (auto &kv : o.onceSize) { uint64_t &m = onceSize[kv.first]; m = std::max(m, kv.second); }
        for (auto &kv : o.writeAt) writeAt.try_emplace(kv.first, kv.second);
        for (auto &kv : o.hotWriteAt) hotWriteAt.try_emplace(kv.first, kv.second);
        for (auto &kv : o.hotReadAt) hotReadAt.try_emplace(kv.first, kv.second);
        if (!firstWrite) firstWrite = o.firstWrite;
        if (!firstAccess) firstAccess = o.firstAccess;
    }
};

// Offsets within one root object, in increasing order of discovery; sorted
// before reporting so the output does not depend on walk order.
using FieldMap = llvm::MapVector<uint64_t, FieldInfo>;

class WTCFalseSharingDiagnosePass
    : public PassWrapper<WTCFalseSharingDiagnosePass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCFalseSharingDiagnosePass)

    WTCFalseSharingDiagnosePass() = default;
    WTCFalseSharingDiagnosePass(const WTCFalseSharingDiagnosePass &other)
        : PassWrapper(other) {}

    StringRef getArgument() const final { return "wtc-false-sharing-diagnose"; }
    StringRef getDescription() const final {
        return "Report pairs of fields written by different threads that share a cache line; "
               "changes nothing";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    Option<unsigned> maxInstances{
        *this, "max-instances",
        llvm::cl::desc("Give up past this many function instances (default 250000)"),
        llvm::cl::init(ThreadInstanceWalk::kInstanceCap)};
    Option<unsigned> lineSize{
        *this, "cache-line",
        llvm::cl::desc("Bytes in a cache line on the target (default 64)"),
        llvm::cl::init(64)};
    // What malloc, calloc and new guarantee on the target: 16 on x86-64
    // glibc. An allocation whose call names its alignment uses that instead.
    Option<unsigned> heapAlign{
        *this, "heap-align",
        llvm::cl::desc("Alignment malloc guarantees on the target (default 16)"),
        llvm::cl::init(16)};

    void runOnOperation() override;

private:
    ModuleOp module;
    // Accesses by place (see PlaceKey): with one element per thread the
    // offsets are within one element and the elements of two neighbouring
    // threads can meet on one line; at an element nobody can name, two
    // accesses may be on one element or on two.
    llvm::MapVector<PlaceKey, FieldMap> byRoot;
    // What the walk knows about a root whose elements were handed out by
    // a spawn loop: the element's size (0 when two handles disagreed, and
    // then the relative offsets mean nothing) and the threads that own an
    // element alone (see Place::owner).
    struct PerThread {
        uint64_t elemSize = 0;
        bool conflict = false;
        llvm::SmallSet<unsigned, 4> owners;
    };
    llvm::DenseMap<Operation *, PerThread> perThread;
    // The smallest element at an unknown index on the root: two accesses
    // at unknown elements are at least this far apart when not on one.
    llvm::DenseMap<Operation *, uint64_t> unknownElem;
    // Per root, the threads with a repeated access at an element of it the
    // analysis cannot name: such a thread may be touching the very field a
    // cold verdict on the root says it touches once.
    llvm::DenseMap<Operation *, llvm::SmallSet<unsigned, 4>> unknownIndexThreads;
    llvm::DenseSet<Operation *> unknownIndexOps;
    // Roots touched by an instance that may run more than once. A stack slot
    // in a function that runs exactly once -- main, above all -- is one
    // object, not one per invocation, so the usual downgrade does not apply
    // to it. That is the commonest textbook shape of this bug: two threads
    // handed the address of a local declared in main.
    llvm::DenseSet<Operation *> multiInvocation;
    // Counted as distinct operations, not per instance: one access reached
    // through a thousand call strings is one access the analysis could not
    // follow, and counting it a thousand times made memcached's figure
    // 1.6 million, more than the program has operations.
    llvm::DenseSet<Operation *> unresolvedOps;
    // Threads with an access the analysis could not place. Such a thread
    // may be touching the very field a cold verdict says it touches once,
    // through a pointer the walk could not follow, so no pair involving it
    // is cold: unknown must not print as "costs nothing".
    llvm::DenseSet<unsigned> unresolvedThreads;
    // Threads with a recurrence the walk visited but did not expand: what
    // those calls reach is unseen, so such a thread may be hot-writing any
    // field, and no pair is cold while one exists besides the once-writer.
    llvm::DenseSet<unsigned> truncatedThreads;
    llvm::DenseSet<Operation *> stackLocalOps;   // slots that never leave their invocation
    llvm::DenseSet<Operation *> threadLocalOps;  // every thread has its own copy
    unsigned definite = 0, possible = 0;
    unsigned coldPairs = 0;   // different threads, but one side touches once
    // Possible pairs that would be cold if the once-side's thread had no
    // unplaced accesses. On real code that is most of what was once "cold":
    // every memcached thread has accesses the walk cannot place, so the
    // count is kept apart from the possibles that are possible for another
    // reason, or "possible" would stop meaning anything there.
    unsigned unplacedPairs = 0;
    size_t walkInstances = 0;

    // Walks the chain of cir.get_member and element steps back to whatever
    // the record or array sits in, accumulating the byte offset. A member
    // of a member contributes both offsets, so a field nested two deep is
    // placed correctly in its line. Returns the base value, or null if the
    // address is neither a member nor an element access.
    //
    // An element step with a constant index -- arr[2].f, s.counts[1] --
    // adds that many elements to the offset. Any other index names an
    // element the analysis cannot read: `unknown` is set, the offset is
    // within that element, and `unknownElemSize` is the size of the
    // smallest such element, the least two such accesses are apart when
    // they are not on one.
    Value resolveMember(Value addr, const DataLayout &dl, uint64_t &offset,
                        std::string &path, bool &unknown, uint64_t &unknownElemSize) const {
        offset = 0;
        unknown = false;
        unknownElemSize = 0;
        llvm::SmallVector<std::string, 4> parts;
        bool stepped = false;
        // Returns false on an element of unknown size.
        auto stepElements = [&](Value &base) {
            for (unsigned steps = 0; steps < 16; ++steps) {
                Value index, next;
                Type elem;
                if (auto ps = base.getDefiningOp<cir::PtrStrideOp>()) {
                    index = ps.getStride(); next = ps.getBase();
                    elem = cast<cir::PointerType>(ps.getBase().getType()).getPointee();
                } else if (auto ge = base.getDefiningOp<cir::GetElementOp>()) {
                    index = ge.getIndex(); next = ge.getBase();
                    auto arr = dyn_cast<cir::ArrayType>(cast<cir::PointerType>(ge.getBase().getType()).getPointee());
                    if (!arr) return false;
                    elem = arr.getElementType();
                } else {
                    return true;
                }
                if (!elem || isa<cir::VoidType>(elem) || isa<cir::FuncType>(elem)) return false;
                stepped = true;
                auto c = traceToLocalRoot(index).getDefiningOp<cir::ConstantOp>();
                auto k = c ? dyn_cast<cir::IntAttr>(c.getValue()) : cir::IntAttr();
                if (!k || k.getValue().isNegative() || k.getValue().getActiveBits() > 32) {
                    // The smallest element on the way: two accesses at
                    // unknown elements may be as close as that.
                    unknown = true;
                    const uint64_t sz = dl.getTypeSize(elem);
                    unknownElemSize = unknownElemSize ? std::min(unknownElemSize, sz) : sz;
                    parts.push_back("[?]");
                } else {
                    offset += k.getValue().getZExtValue() * dl.getTypeSize(elem);
                    parts.push_back("[" + std::to_string(k.getValue().getZExtValue()) + "]");
                }
                base = traceToLocalRoot(next);
            }
            return false;
        };
        auto finish = [&] {
            for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                if (!path.empty() && (*it)[0] != '[') path += ".";
                path += *it;
            }
            // An element of an array of scalars has no name but its index.
            if (path.empty()) path = "[0]";
            else if (path == "[?]") path = "<element>";
        };
        Value cur = traceToLocalRoot(addr);
        if (!stepElements(cur)) return Value();
        auto gm = cur.getDefiningOp<cir::GetMemberOp>();
        if (!gm) {
            if (!stepped && !decaysArray(addr)) return Value();
            finish();
            return cur;
        }
        unsigned guard = 0;
        while (gm && guard++ < 64) {
            auto ptrTy = dyn_cast<cir::PointerType>(gm.getAddr().getType());
            if (!ptrTy) return Value();
            auto rec = dyn_cast<cir::RecordType>(ptrTy.getPointee());
            if (!rec || rec.isIncomplete()) return Value();
            offset += rec.getElementOffset(dl, gm.getIndex());
            parts.push_back(gm.getName().str());
            Value base = traceToLocalRoot(gm.getAddr());
            if (!stepElements(base)) return Value();
            auto outer = base.getDefiningOp<cir::GetMemberOp>();
            if (!outer) {
                finish();
                return base;
            }
            gm = outer;
        }
        return Value();
    }

    void foldPlaces();
    void report(const ThreadInstanceWalk &walk);

    // Whether an address is an array's first element, reached by letting the
    // array decay to a pointer with no step after it: `counts[0]`, or the
    // array passed whole to memset.
    static bool decaysArray(Value addr) {
        Value v = addr;
        for (unsigned guard = 0; guard < 16; ++guard) {
            auto c = v.getDefiningOp<cir::CastOp>();
            if (!c) return false;
            if (c.getKind() == cir::CastKind::array_to_ptrdecay) return true;
            if (c.getKind() != cir::CastKind::bitcast) return false;
            v = c.getSrc();
        }
        return false;
    }

    // What an address is, read backwards through casts: a member chain, an
    // element of an array, the whole of an object reached through a cast of
    // its address, or none of these.
    enum class Shape { None, Member, Array, Whole };
    static Shape shapeOf(Value addr) {
        Value v = traceToLocalRoot(addr);
        bool array = decaysArray(addr);
        for (unsigned guard = 0; guard < 64; ++guard) {
            // A member reached through an element step, s.counts[1], is a
            // member access: resolveMember reads the index, and refuses it
            // when it is not a constant.
            if (v.getDefiningOp<cir::GetMemberOp>()) return Shape::Member;
            if (auto ps = v.getDefiningOp<cir::PtrStrideOp>()) { array = true; v = traceToLocalRoot(ps.getBase()); continue; }
            if (auto ge = v.getDefiningOp<cir::GetElementOp>()) { array = true; v = traceToLocalRoot(ge.getBase()); continue; }
            break;
        }
        if (array) return Shape::Array;
        // Only a record: a scalar has no second field to pair with, and
        // counting every function with a local scalar as relevant brought
        // the standard library's call graph back into the walk.
        Value t = traceToRoot(addr);
        Type ty;
        // Through a pointer that came from elsewhere -- a parameter, a load,
        // a call -- the pointee is what is there: the walk may still place
        // it, as a heap object or a per-thread element, and a scalar
        // element is the textbook per-thread counter.
        if (isa<BlockArgument>(t) || isa_and_nonnull<cir::LoadOp, cir::CallOp>(t.getDefiningOp())) {
            if (auto pt = dyn_cast<cir::PointerType>(t.getType())) {
                Type pe = pt.getPointee();
                if (isa<cir::RecordType, cir::IntType, cir::PointerType, cir::SingleType, cir::DoubleType>(pe))
                    return Shape::Whole;
            }
            return Shape::None;
        }
        if (auto gg = t.getDefiningOp<cir::GetGlobalOp>()) {
            // An alias may declare a type of its own; the object's is the
            // aliasee's.
            auto g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr());
            for (unsigned hop = 0; g && g.getAliasee() && hop < 8; ++hop)
                g = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(
                    g, StringAttr::get(g.getContext(), *g.getAliasee()));
            ty = g ? g.getSymType() : gg.getType().getPointee();
        } else if (auto a = t.getDefiningOp<cir::AllocaOp>()) ty = a.getAllocaType();
        if (ty && isa<cir::RecordType>(ty)) return Shape::Whole;
        return Shape::None;
    }

    // The pointer an indexed address starts from, with the strides and
    // element steps removed: what resolves to the array's root.
    static Value arrayBase(Value addr) {
        Value v = traceToLocalRoot(addr);
        for (unsigned guard = 0; guard < 64; ++guard) {
            if (auto ps = v.getDefiningOp<cir::PtrStrideOp>()) { v = traceToLocalRoot(ps.getBase()); continue; }
            if (auto ge = v.getDefiningOp<cir::GetElementOp>()) { v = traceToLocalRoot(ge.getBase()); continue; }
            if (auto gm = v.getDefiningOp<cir::GetMemberOp>()) { v = traceToLocalRoot(gm.getAddr()); continue; }
            break;
        }
        return v;
    }

    static Type rootType(Operation *root) {
        if (auto g = dyn_cast<cir::GlobalOp>(root)) return g.getSymType();
        if (auto a = dyn_cast<cir::AllocaOp>(root)) return a.getAllocaType();
        return Type();
    }

    // A heap object has no declared type: the accesses say what it is, and
    // the allocator says how it is aligned.
    static bool isHeapRoot(Operation *root) { return isa<cir::CallOp>(root); }

    // Whether the data layout can size and align the object a root names.
    static bool hasLayout(Operation *root) {
        if (isHeapRoot(root)) return true;
        Type ty = rootType(root);
        if (!ty) return false;
        auto rec = dyn_cast<cir::RecordType>(ty);
        return !rec || !rec.isIncomplete();
    }

    // A stack slot is a different object in every invocation, so two threads
    // running the same function do not share it and cannot falsely share its
    // fields. Running on real compiler output, this was the whole of the
    // pass's noise: libstdc++ locals such as allocated_ptr's __guard_ptr,
    // reported because the function they live in is reachable from many
    // threads. They are only interesting if the address is published, and
    // even then two invocations are two objects, so an escaping slot can
    // reach "possible" and never "definite".
    static bool addressEscapes(cir::AllocaOp slot) {
        llvm::SmallVector<Value, 8> work{slot.getResult()};
        llvm::DenseSet<Operation *> seen;
        unsigned guard = 0;
        while (!work.empty() && guard++ < 512) {
            Value v = work.pop_back_val();
            for (Operation *u : v.getUsers()) {
                // A store is judged per operand: the same store may hold
                // the slot's address as its destination and, through another
                // derived value, as the value stored, and only the second
                // publishes it.
                if (auto st = dyn_cast<cir::StoreOp>(u)) {
                    if (st.getValue() == v) return true;
                    continue;
                }
                if (!seen.insert(u).second) continue;
                if (isa<cir::LoadOp>(u)) continue;                  // reads the slot
                if (isa<cir::CastOp, cir::GetMemberOp, cir::GetElementOp, cir::PtrStrideOp>(u)) {
                    work.push_back(u->getResult(0));
                    continue;
                }
                return true;    // a call, a global initialiser, anything else
            }
        }
        // Out of budget with work left: assume the worst. With the work
        // done, the address went nowhere. An earlier version returned true
        // on both exits, so no slot was ever private and the summary's
        // "on stack slots that never escape" was always zero.
        return !work.empty();
    }
};

void WTCFalseSharingDiagnosePass::runOnOperation() {
    module = getOperation();
    byRoot.clear();
    perThread.clear();
    unknownElem.clear();
    unknownIndexThreads.clear();
    unknownIndexOps.clear();
    multiInvocation.clear();
    unresolvedOps.clear();
    stackLocalOps.clear();
    unresolvedThreads.clear();
    truncatedThreads.clear();
    threadLocalOps.clear();
    definite = possible = coldPairs = unplacedPairs = 0;

    const DataLayout dl(module);

    // A function that touches no memory through a record member cannot
    // contribute a field pair, so it needs no instance. Without a pruning
    // predicate the standard library's own call graph fills the worklist.
    ThreadInstanceWalk *self = nullptr;
    auto touchesMember = [](Operation *op) {
        llvm::SmallVector<MemoryAccess, 2> accesses;
        memoryAccesses(op, accesses);
        for (const MemoryAccess &ma : accesses)
            if (shapeOf(ma.addr) != Shape::None) return true;
        return false;
    };
    auto relevant = [&](cir::FuncOp f) { return self->reachesOp(f, touchesMember); };
    ThreadInstanceWalk walk(module, relevant, maxInstances);
    self = &walk;

    walk.run(
        [&](Instance &inst) {
            // A recurrence visited and not expanded: what its calls reach
            // is not in the walk, so this thread may touch any object.
            if (inst.truncated) { unresolvedThreads.insert(inst.thread); truncatedThreads.insert(inst.thread); }
            inst.func.walk([&](Operation *op) {
                auto record = [&](Value addr, bool isWrite, Type valTy) {
                const Shape shape = shapeOf(addr);
                if (shape == Shape::None) return;
                // How often this access runs, decided before the object is
                // resolved, because an access the walk cannot place matters
                // only if it repeats: one touch through an unknown pointer
                // cannot make a side hot, and a touch during start-up
                // precedes every thread.
                //
                // Three answers, not two. An instance that runs many times,
                // or stands for many threads, repeats whatever its control
                // flow looks like; a site inside a loop repeats; otherwise
                // the site's count is a guess wherever the function, or a
                // caller on the way to it, has a goto. A goto used to make
                // every access below it "repeated", so one store followed
                // by a goto was reported as a definite pair.
                // Whether start-up has ended here can itself be a guess: a
                // goto in main, or in a caller that spawns, may jump over the
                // spawn or back behind it. A guess is neither start-up nor
                // after it, so the access is rated unknown.
                const bool startupGuess = walk.startupUncertain(inst);
                const bool startup = !startupGuess && walk.inStartupPhase(op, inst);
                const bool manyAnyway = (inst.threadMult == Mult::Many || inst.execMult == Mult::Many) &&
                                        !inst.execUnknown;
                bool siteKnown;
                const Mult site = walk.siteMult(op, inst.func, siteKnown);
                bool repeated = !startup && !startupGuess &&
                                (manyAnyway || (siteKnown && site == Mult::Many));
                bool unknownRate = !startup && !repeated &&
                                   (startupGuess || !siteKnown || inst.execUnknown);
                auto unplaced = [&] {
                    unresolvedOps.insert(op);
                    if (repeated || unknownRate) unresolvedThreads.insert(inst.thread);
                };

                uint64_t offset = 0;
                std::string path;
                bool unknownIdx = false;
                uint64_t unknownSize = 0;
                Place place;
                // A local array whose address never leaves its invocation:
                // nobody else can be looking at it.
                if (shape == Shape::Array)
                    if (auto slot = dyn_cast_or_null<cir::AllocaOp>(
                            walk.resolveRoot(arrayBase(addr), &inst)))
                        if (!addressEscapes(slot)) { stackLocalOps.insert(op); return; }
                if (shape == Shape::Member || shape == Shape::Array) {
                    // An element at an index the analysis cannot read is
                    // still an element of a known array: the access is
                    // placed on the array, at its offset within some
                    // element. It used to be counted as unplaced, and two
                    // per-thread counters in one array once produced no
                    // output at all.
                    Value base = resolveMember(addr, dl, offset, path, unknownIdx, unknownSize);
                    if (!base) { unplaced(); return; }
                    place = walk.resolvePlace(base, &inst);
                } else {
                    // The whole object through a cast of its address: `*(long
                    // *)&s`, or through a pointer the walk resolves. Of a
                    // global or a slot only a record is of interest; a
                    // scalar global has no second field to pair with.
                    place = walk.resolvePlace(addr, &inst);
                    if (place.root && !isHeapRoot(place.root) && !place.loop && !place.unknownIndex &&
                        !isa<cir::RecordType>(rootType(place.root)))
                        return;
                    path = "<whole>";
                }
                Operation *root = place.root;
                if (!root) { unplaced(); return; }
                // Element steps the walk read on the way to the root: a
                // constant index at the spawn, `&args[1]`, is so many bytes
                // in; an index it could not read makes the element unknown.
                offset += place.offset;
                if (place.unknownIndex) {
                    unknownIdx = true;
                    if (place.elem) {
                        const uint64_t sz = dl.getTypeSize(place.elem);
                        unknownSize = unknownSize ? std::min(unknownSize, sz) : sz;
                    }
                }
                // With one element per thread, what repeats on the thread's
                // own element is what repeats inside the thread: the loop
                // that spawned it stands for many threads, one per element,
                // not for many touches of this one.
                if (place.loop && place.owner && !startup && !startupGuess) {
                    bool wtKnown;
                    const Mult wt = walk.multWithinThread(inst, wtKnown);
                    repeated = (siteKnown && site == Mult::Many) || (wtKnown && wt == Mult::Many);
                    unknownRate = !repeated && (!siteKnown || !wtKnown);
                }
                // A root declared with an incomplete record type (an opaque
                // global the program only ever casts) has no layout to place
                // fields in, and asking the data layout for one asserts.
                if (!hasLayout(root)) { unplaced(); return; }
                // A thread-local global is one object per thread, so no two
                // threads share its line. The walk hands every thread the
                // same GlobalOp for it, which once made its fields a pair.
                if (auto g = dyn_cast<cir::GlobalOp>(root))
                    if (g.getTlsModelAttr()) { threadLocalOps.insert(op); return; }
                if (auto slot = dyn_cast<cir::AllocaOp>(root)) {
                    if (!addressEscapes(slot)) { stackLocalOps.insert(op); return; }
                }

                PlaceKind kind = PlaceKind::Absolute;
                if (unknownIdx) {
                    kind = PlaceKind::Unknown;
                    unknownIndexOps.insert(op);
                    if (repeated || unknownRate) unknownIndexThreads[root].insert(inst.thread);
                    uint64_t &u = unknownElem[root];
                    if (unknownSize) u = u ? std::min(u, unknownSize) : unknownSize;
                } else if (place.loop) {
                    kind = PlaceKind::Relative;
                    PerThread &pt = perThread[root];
                    const uint64_t sz = place.elem && !isa<cir::VoidType, cir::FuncType>(place.elem)
                                            ? dl.getTypeSize(place.elem) : 0;
                    if (!sz || (pt.elemSize && pt.elemSize != sz)) pt.conflict = true;
                    else pt.elemSize = sz;
                    if (place.owner) pt.owners.insert(inst.thread);
                }
                const PlaceKey key{root, kind};
                FieldInfo &fi = byRoot[key][offset];
                // The most specific name seen: a member's over the record's,
                // and a record's over "<whole>".
                if (fi.name.empty() || fi.name == "<whole>" ||
                    (path != "<whole>" && path.size() > fi.name.size()))
                    fi.name = path;
                if (!fi.firstAccess) fi.firstAccess = op;
                uint64_t size = 1;
                if (valTy && !isa<cir::VoidType, cir::FuncType>(valTy)) size = dl.getTypeSize(valTy);
                fi.size = std::max<uint64_t>(fi.size, size);
                // The once-only size is the size of the once-only *write*:
                // the cold rule pairs a once-writer, and a once-only read of
                // the whole record by the same thread must not widen it.
                if (repeated || unknownRate) {
                    uint64_t &mine = fi.hotSize[inst.thread];
                    mine = std::max(mine, size);
                } else if (isWrite) {
                    uint64_t &mine = fi.onceSize[inst.thread];
                    mine = std::max(mine, size);
                }
                if (!inst.speculative) fi.certain.insert(inst.thread);
                if (isWrite) {
                    fi.writers.insert(inst.thread);
                    fi.writeAt.try_emplace(inst.thread, op);
                    if (repeated) fi.hotWriters.insert(inst.thread);
                    if (unknownRate) fi.unknownRateWriters.insert(inst.thread);
                    if (repeated || unknownRate) fi.hotWriteAt.try_emplace(inst.thread, op);
                    if (repeated && inst.threadMult == Mult::Many) fi.manyHotWriters.insert(inst.thread);
                    if (!fi.firstWrite) fi.firstWrite = op;
                } else {
                    fi.readers.insert(inst.thread);
                    if (repeated) fi.hotReaders.insert(inst.thread);
                    if (unknownRate) fi.unknownRateReaders.insert(inst.thread);
                    if (repeated || unknownRate) fi.hotReadAt.try_emplace(inst.thread, op);
                }
                };
                llvm::SmallVector<MemoryAccess, 2> accesses;
                memoryAccesses(op, accesses);
                for (const MemoryAccess &ma : accesses) record(ma.addr, ma.isWrite, ma.type);
            });
        },
        [](cir::FuncOp) { /* recursion: the walk visits the instance and stops expanding */ });

    if (walk.capExceeded()) {
        // Past the cap the walk stops adding instances, so every verdict
        // below would be computed from a call graph that is missing part of
        // the program. Silence is the only sound answer.
        module.emitRemark() << "wtc: gave up after " << walk.cap()
                            << " function instances; no false-sharing verdict is given for "
                               "this module";
        markAllAnalysesPreserved();
        return;
    }
    walkInstances = walk.instances();
    // A stack slot is one object only when the function that owns it ran
    // once: one instance, once, on one thread. The instance that touched
    // the slot may well have run once, from a function that called it
    // twice -- two calls, two slots -- and, the other way round, a worker
    // spawned in a loop touching main's slot does not multiply the slot.
    for (auto &kv : byRoot) {
        if (auto slot = dyn_cast<cir::AllocaOp>(kv.first.root))
            if (!walk.runsOnce(slot->getParentOfType<cir::FuncOp>()))
                multiInvocation.insert(kv.first.root);
        // An allocation site that may run more than once -- in a loop, or
        // in a function the call graph does not show to run once -- stands
        // for many objects, and two threads on two of its fields may be on
        // two objects. The site is the allocator call, often inside a
        // wrapper that has no instance of its own, so the answer comes
        // from the call graph rather than from the walk's instances.
        if (isa<cir::CallOp>(kv.first.root) && !walk.siteRunsOnce(kv.first.root))
            multiInvocation.insert(kv.first.root);
    }
    foldPlaces();
    report(walk);
    markAllAnalysesPreserved();
}

// Brings the accesses on one root to one frame of reference. Where a spawn
// loop handed out elements of a known size, an access at a constant index
// -- main reading `args[0].sum` after the join, a worker touching
// `args[1]` handed to it directly -- is in some element, at the offset
// modulo the element size, and meets the per-thread accesses there. Where
// the element size is not known, the per-thread accesses are at an element
// nobody can name.
void WTCFalseSharingDiagnosePass::foldPlaces() {
    for (auto &kv : perThread) {
        Operation *root = kv.first;
        const PerThread &pt = kv.second;
        const PlaceKey relative{root, PlaceKind::Relative};
        auto rel = byRoot.find(relative);
        if (rel == byRoot.end()) continue;
        if (pt.conflict || !pt.elemSize) {
            FieldMap &dst = byRoot[PlaceKey{root, PlaceKind::Unknown}];
            for (auto &f : rel->second) dst[f.first].absorb(f.second);
            byRoot.erase(relative);
            continue;
        }
        auto abs = byRoot.find(PlaceKey{root, PlaceKind::Absolute});
        if (abs == byRoot.end()) continue;
        FieldMap &dst = byRoot[relative];
        for (auto &f : abs->second) {
            // The element's index is folded into the offset, so it leaves
            // the name: `[0].head` is the element's `head`.
            FieldInfo moved = f.second;
            if (!moved.name.empty() && moved.name[0] == '[') {
                const size_t close = moved.name.find(']');
                if (close != std::string::npos) {
                    moved.name = moved.name.substr(close + 1);
                    if (!moved.name.empty() && moved.name[0] == '.') moved.name.erase(0, 1);
                    if (moved.name.empty()) moved.name = "<whole>";
                }
            }
            dst[f.first % pt.elemSize].absorb(moved);
        }
        byRoot.erase(PlaceKey{root, PlaceKind::Absolute});
    }
}

void WTCFalseSharingDiagnosePass::report(const ThreadInstanceWalk &walk) {
    const uint64_t line = lineSize;
    const DataLayout dl(module);

    for (auto &[key, fields] : byRoot) {
        Operation *root = key.root;
        const bool relative = key.kind == PlaceKind::Relative;
        const bool unknownIndex = key.kind == PlaceKind::Unknown;
        const uint64_t elemSize = relative ? perThread.lookup(root).elemSize : 0;
        const uint64_t unknownSize = unknownIndex ? unknownElem.lookup(root) : 0;
        const llvm::SmallSet<unsigned, 4> noOwners;
        const llvm::SmallSet<unsigned, 4> &owners = relative ? perThread.find(root)->second.owners : noOwners;
        auto hasUnknownIndex = [&](unsigned t) {
            auto it = unknownIndexThreads.find(root);
            return it != unknownIndexThreads.end() && it->second.count(t) > 0;
        };
        // Sorted by offset so the report reads like the struct does, and so
        // it does not depend on the order the walk happened to find them.
        llvm::SmallVector<uint64_t, 16> offsets;
        for (auto &kv : fields) offsets.push_back(kv.first);
        llvm::sort(offsets);

        std::string label;
        // A slot of a function that runs more than once, or a heap object
        // whose allocation runs more than once, is many objects: two threads
        // on two fields of "it" may be on two of them.
        const bool onStack = (isa<cir::AllocaOp>(root) || isHeapRoot(root)) && multiInvocation.count(root);
        if (auto g = dyn_cast<cir::GlobalOp>(root)) label = ("@" + g.getSymName()).str();
        else if (auto a = dyn_cast<cir::AllocaOp>(root)) label = ("%" + a.getName()).str();
        else if (auto call = dyn_cast<cir::CallOp>(root)) {
            cir::FuncOp callee = lookupCallee(call);
            label = ((callee ? callee.getSymName() : StringRef("allocation")) + "() in @" +
                     call->getParentOfType<cir::FuncOp>().getSymName()).str();
        } else label = "<object>";
        if (relative) label += " (one element per thread)";
        if (unknownIndex) label += " (element not known)";

        // Where the object may start relative to a line. Two fields share a
        // line only for some starting positions unless the object is
        // line-aligned: in a 56-byte struct aligned to 8, the first and the
        // last field share a line when it starts at a line boundary and do
        // not when it starts 16 bytes in. An earlier version assumed every
        // object starts on a boundary, which reported pairs the linker may
        // have put on two lines without saying so, and missed pairs that
        // only meet across a boundary.
        //
        // The verdict -- definite or possible -- stays a statement about
        // threads. Where the pair lands is the linker's choice, so the
        // report says in how many of the allowed positions the two fields
        // share a line, rather than folding it into the verdict: otherwise
        // two adjacent counters in an 8-aligned struct, which share a line
        // in seven positions of eight, could never be definite.
        uint64_t align = 1;
        if (auto g = dyn_cast<cir::GlobalOp>(root))
            align = g.getAlignment().value_or(dl.getTypeABIAlignment(g.getSymType()));
        else if (auto a = dyn_cast<cir::AllocaOp>(root))
            align = a.getAlignment() ? a.getAlignment() : dl.getTypeABIAlignment(a.getAllocaType());
        else if (auto call = dyn_cast<cir::CallOp>(root))
            // What the allocator promises: the call's own alignment operand,
            // else the platform's guarantee for malloc.
            align = allocationAlignment(call).value_or(heapAlign);
        align = std::max<uint64_t>(align, 1);
        // Where an element may start: at the array's alignment, stepped by
        // the element size.
        const uint64_t startStep = relative && elemSize ? std::gcd(align, elemSize)
                                   : unknownIndex && unknownSize ? std::gcd(align, unknownSize)
                                                                 : align;
        auto sharesAt = [&](uint64_t oa, uint64_t sa, uint64_t ob, uint64_t sb, uint64_t shift) {
            const uint64_t a0 = (oa + shift) / line, a1 = (oa + shift + std::max<uint64_t>(sa, 1) - 1) / line;
            const uint64_t b0 = (ob + shift) / line, b1 = (ob + shift + std::max<uint64_t>(sb, 1) - 1) / line;
            return a0 <= b1 && b0 <= a1;
        };

        // Geometry is judged per pair of accesses, with the sizes of the two
        // accesses that make the verdict. Overlapping bytes are the same data:
        // a store of a whole record against a store to one of its members is
        // true sharing, which no layout removes.
        auto sizeOf = [](const FieldInfo &f, unsigned t, bool hot) -> uint64_t {
            const auto &m = hot ? f.hotSize : f.onceSize;
            auto it = m.find(t);
            return it == m.end() ? 1 : std::max<uint64_t>(it->second, 1);
        };
        auto overlap = [](uint64_t ox, uint64_t sx, uint64_t oy, uint64_t sy) {
            return ox < oy + sy && oy < ox + sx;
        };
        auto geometry = [&](uint64_t ox, uint64_t sx, uint64_t oy, uint64_t sy,
                            unsigned &positions, unsigned &sharing) {
            positions = sharing = 0;
            for (uint64_t shift = 0; shift < line; shift += std::min(startStep, line)) {
                ++positions;
                sharing += sharesAt(ox, sx, oy, sy, shift);
            }
            return sharing > 0;
        };
        // How the report names the positions: an object starts where its
        // alignment lets it; an element where the array's alignment and
        // the element size let it.
        auto positionsText = [&](unsigned sharing, unsigned positions) {
            std::string n = " in " + std::to_string(sharing) + " of the " + std::to_string(positions) + " positions ";
            if (relative || (unknownIndex && unknownSize))
                return n + "an element of " + std::to_string(relative ? elemSize : unknownSize) + " bytes may start at";
            return n + "an object aligned to " + std::to_string(align) + " bytes may start at";
        };

        for (size_t i = 0; i < offsets.size(); ++i) {
            // At elements nobody can name the same field pairs with itself:
            // in two neighbouring elements it sits on one line.
            for (size_t j = unknownIndex ? i : i + 1; j < offsets.size(); ++j) {
                const uint64_t oa = offsets[i], ob = offsets[j];
                const FieldInfo &a = fields[oa];
                const FieldInfo &b = fields[ob];
                unsigned positions = 0, sharing = 0;

                // False sharing needs two different threads on the same line
                // with at least one write. Two threads on the *same* field is
                // true sharing: a race or a counter the program synchronises,
                // and either way not a layout question.
                // Sustained traffic needs a repeated write on one field and a
                // repeated access on the other, from a different thread.
                // Checked against memcached's `settings`, where the analysis
                // first reported a pair whose write is `settings.lru_
                // maintainer_thread = false` on the shutdown path: one store,
                // once, for one invalidation and no cost. The neighbouring
                // project measured the same shape and called it what it is,
                // dynamically cold.
                bool hit = false;
                // The two accesses that make the verdict, kept so the report
                // can name both.
                Operation *whereA = nullptr, *whereB = nullptr;
                auto pick = [&](const FieldInfo &owner, unsigned t) -> Operation * {
                    if (Operation *o = owner.hotWriteAt.lookup(t)) return o;
                    return owner.hotReadAt.lookup(t);
                };
                // Whether the accesses of w on x and u on y, at offsets ox and
                // oy, are two pieces of data on one line.
                auto lands = [&](const FieldInfo &x, uint64_t ox, unsigned w, bool wHot,
                                 const FieldInfo &y, uint64_t oy, unsigned u) {
                    const uint64_t sx = sizeOf(x, w, wHot), sy = sizeOf(y, u, true);
                    const bool same = overlap(ox, sx, oy, sy);
                    if (!same && geometry(ox, sx, oy, sy, positions, sharing)) return true;
                    if (!unknownIndex || !unknownSize) return false;
                    // Two accesses at elements nobody can name may be in
                    // neighbouring elements, where the end of one meets the
                    // start of the next on a line. Positions are not
                    // counted there; the report says "neighbouring".
                    unsigned p = 0, s = 0;
                    if (!geometry(ox, sx, oy + unknownSize, sy, p, s) &&
                        !geometry(oy, sy, ox + unknownSize, sx, p, s))
                        return false;
                    positions = sharing = 0;
                    return true;
                };
                auto tryPair = [&](const FieldInfo &x, uint64_t ox, const FieldInfo &y, uint64_t oy,
                                   Operation *&wx, Operation *&wy) {
                    for (unsigned w : x.hotWriters) {
                        if (!x.certain.count(w)) continue;
                        for (const auto &set : {y.hotWriters, y.hotReaders})
                            for (unsigned u : set)
                                if (u != w && y.certain.count(u) && !walk.exclusive(w, u) &&
                                    lands(x, ox, w, true, y, oy, u)) {
                                    hit = true;
                                    if (!wx) { wx = pick(x, w); wy = pick(y, u); }
                                    return;
                                }
                    }
                };
                tryPair(a, oa, b, ob, whereA, whereB);
                if (!hit) tryPair(b, ob, a, oa, whereB, whereA);
                // At elements nobody can name, two threads on two fields may
                // be on one element or on two, and on the same field on two
                // neighbouring elements or on one, which is true sharing. A
                // statement about threads, not about a line: possible, never
                // definite, and never cold.
                const bool unknownHit = unknownIndex && hit;
                if (unknownHit) hit = false;

                // A thread standing for many could be two threads, one on
                // each field, and a thread the module cannot account for
                // could be any thread. Possible is still a statement about
                // sustained traffic, so it takes the same repeated accesses a
                // definite verdict does: a repeated write on one field, a
                // repeated access on the other. An earlier version asked only
                // whether anything had ever been written, and paired
                // memcached's option parsing -- start-up, run once -- with a
                // command that rewrites the same setting.
                // A side whose rate is unknown may be repeated, so it can
                // make a pair possible; it cannot make one definite or cold.
                // The reason is kept, so the remark can say which.
                const char *why = nullptr;
                bool contendSameThread = false;
                auto mayContend = [&](const FieldInfo &x, uint64_t ox, const FieldInfo &y, uint64_t oy,
                                      Operation *&wx, Operation *&wy) {
                    for (const auto &xs : {x.hotWriters, x.unknownRateWriters})
                        for (unsigned w : xs)
                            for (const auto &set : {y.hotWriters, y.hotReaders,
                                                    y.unknownRateWriters, y.unknownRateReaders})
                                for (unsigned u : set) {
                                    // One thread standing for many on two fields of
                                    // one element: when each of those threads owns
                                    // its element alone, that is one thread, and no
                                    // pair.
                                    if (u == w && (!x.manyHotWriters.count(w) || owners.count(w))) continue;
                                    if (!lands(x, ox, w, true, y, oy, u)) continue;
                                    wx = pick(x, w); wy = pick(y, u);
                                    contendSameThread = u == w;
                                    if (u == w)
                                        why = "by one thread that stands for many, which may be one "
                                              "thread on both fields or two";
                                    else if (!x.certain.count(w) || !y.certain.count(u))
                                        why = "by a thread the analysis cannot account for";
                                    else if (walk.exclusive(w, u))
                                        why = "by two threads the analysis cannot show both exist";
                                    else
                                        why = "by a thread whose access rate the analysis cannot tell";
                                    return true;
                                }
                    return false;
                };
                const bool contend = unknownHit || (!hit && (mayContend(a, oa, b, ob, whereA, whereB) ||
                                                             mayContend(b, ob, a, oa, whereB, whereA)));
                if (contend && unknownIndex)
                    why = "which element each is at the analysis cannot read, so the two may be "
                          "on one element or on two";

                // Cold means exactly one thing: accounted-for threads on both
                // sides, at least one of which touches the line only once,
                // and nothing else that might keep it travelling. It is a
                // *resolved* answer -- "this costs nothing" -- so it comes
                // last: a pair that may contend through a thread the module
                // cannot account for is unknown, and an earlier version let
                // one extra store by main turn such a pair into "costs
                // nothing". Unknown must print as unknown.
                //
                // A once-writer whose thread has an access the walk could not
                // place may be writing this very field through the pointer it
                // could not follow. One such writer makes the pair unknown,
                // whatever the other writers look like: an earlier version
                // stopped at the first writer it could vouch for.
                bool cold = false, unplaced = false;
                unsigned chosenPositions = 0, chosenSharing = 0;
                if (!hit && !contend && !unknownIndex) {
                    auto coldAgainst = [&](const FieldInfo &x, uint64_t ox, const FieldInfo &y, uint64_t oy,
                                           Operation *&wx, Operation *&wy) {
                        bool any = false;
                        for (unsigned w : x.writers) {
                            if (!x.certain.count(w)) continue;
                            if (x.hotWriters.count(w)) continue;   // this side is not cold
                            if (x.unknownRateWriters.count(w)) continue;   // nor known to be
                            for (const auto &set : {y.hotWriters, y.hotReaders})
                                for (unsigned u : set) {
                                    if (u == w || !y.certain.count(u)) continue;
                                    if (!lands(x, ox, w, false, y, oy, u)) continue;
                                    // The report names the once-writer's own
                                    // store and the geometry of this very pair.
                                    bool unplacedHere = unresolvedThreads.count(w) > 0 || hasUnknownIndex(w);
                                    for (unsigned t : truncatedThreads) unplacedHere |= t != w;
                                    if ((unplacedHere && !unplaced) || (!unplacedHere && !any && !unplaced)) {
                                        wx = x.writeAt.lookup(w); wy = pick(y, u);
                                        chosenPositions = positions; chosenSharing = sharing;
                                    }
                                    if (unplacedHere) unplaced = true; else any = true;
                                }
                        }
                        return any;
                    };
                    const bool ab = coldAgainst(a, oa, b, ob, whereA, whereB);
                    const bool ba = coldAgainst(b, ob, a, oa, whereB, whereA);
                    cold = (ab || ba) && !unplaced;
                    positions = chosenPositions; sharing = chosenSharing;
                }
                if (!hit && !contend && !cold && !unplaced) continue;   // never on one line

                const bool shareAll = sharing == positions;
                std::string where;
                if (!shareAll) where = positionsText(sharing, positions);
                else if (unknownIndex && positions == 0)
                    where = " when the two land in neighbouring elements";

                // The address was published, so two threads may be looking
                // at it -- but they may equally be looking at two
                // invocations, or two allocations, which are two objects.
                // Not a claim; said, rather than dropped, whenever the two
                // sides are two threads. One thread standing for many on
                // both fields of a slot is one invocation writing its own
                // copy, and saying anything about it is noise: it is what
                // libstdc++'s own locals produced on real output.
                if (onStack && (hit || unplaced || (contend && !contendSameThread))) {
                    ++possible;
                    Operation *at = whereA ? whereA : (a.firstWrite ? a.firstWrite : a.firstAccess);
                    auto d = at->emitRemark() << "wtc: possible false sharing in " << label << ": '";
                    if (oa == ob) d << a.name << "' of two elements may share one " << line << "-byte line";
                    else d << a.name << "' and '" << b.name << "' share one " << line << "-byte line" << where;
                    d << (isHeapRoot(root)
                              ? ", but the allocation may run more than once, so two threads "
                                "may be looking at two objects rather than one"
                              : ", but the object is a stack slot whose address escapes, "
                                "so two threads may be looking at two invocations rather than one");
                    continue;
                }
                if (hit) {
                    ++definite;
                    Operation *at = whereA ? whereA : (a.firstWrite ? a.firstWrite : a.firstAccess);
                    auto d = at->emitRemark()
                        << "wtc: false sharing in " << label << ": '" << a.name
                        << "' (offset " << oa << ") and '" << b.name << "' (offset " << ob
                        << ") are touched by different threads and share one " << line
                        << "-byte line" << where
                        << "; separating them costs nothing if the fields can be reordered";
                    if (whereB) d.attachNote(whereB->getLoc())
                        << "wtc: the other thread touches '" << b.name << "' here";
                    continue;
                }
                if (contend && !onStack) {
                    ++possible;
                    auto d = whereA->emitRemark() << "wtc: possible false sharing in " << label << ": '";
                    if (oa == ob)
                        d << a.name << "' of two elements may share one " << line
                          << "-byte line and is touched repeatedly, but " << why;
                    else
                        d << a.name << "' and '" << b.name << "' share one " << line << "-byte line"
                          << where << " and are touched repeatedly, but " << why;
                    if (whereB) d.attachNote(whereB->getLoc())
                        << "wtc: the other access to '" << b.name << "' is here";
                    continue;
                }
                if (unplaced && !onStack) {
                    ++possible;
                    ++unplacedPairs;
                    Operation *at = whereA ? whereA : (a.firstWrite ? a.firstWrite : a.firstAccess);
                    auto d = at->emitRemark()
                        << "wtc: possible false sharing in " << label << ": '" << a.name
                        << "' and '" << b.name << "' share one " << line << "-byte line" << where
                        << "; one side touches it once as far as the analysis can see, but "
                           "that thread also has accesses it could not place";
                    if (whereB) d.attachNote(whereB->getLoc())
                        << "wtc: the other access to '" << b.name << "' is here";
                    continue;
                }
                if (cold) ++coldPairs;
            }
        }

        // With an element per thread, the elements of two neighbouring
        // threads are adjacent in memory, and the tail of one shares a line
        // with the head of the next unless the element is a multiple of a
        // line and the array starts on one. A thread that owns its element
        // alone (Place::owner: the loop steps by one and spawns every time,
        // so both neighbours have a thread) hot-writes field a of its own
        // element and hot-touches field b; across the boundary that is two
        // different threads, so the pair is definite; with a once-only write
        // of a it is cold, on the same terms as within an object. The loop
        // is read as written, a loop; one that runs once starts one thread
        // and makes the pair moot rather than wrong. This is the shape of
        // Phoenix's linear_regression and of memcached's threads[]: one
        // struct per thread, allocated as an array.
        if (relative && elemSize && !owners.empty()) {
            auto pick = [](const FieldInfo &owner, unsigned t) -> Operation * {
                if (Operation *o = owner.hotWriteAt.lookup(t)) return o;
                return owner.hotReadAt.lookup(t);
            };
            for (uint64_t oa : offsets) {
                for (uint64_t ob : offsets) {
                    const FieldInfo &a = fields[oa], &b = fields[ob];
                    unsigned w = 0;
                    bool hot = false, once = false;
                    for (unsigned t : a.writers) {
                        if (!owners.count(t) || !a.certain.count(t) || !b.certain.count(t)) continue;
                        if (!b.hotWriters.count(t) && !b.hotReaders.count(t)) continue;
                        if (a.hotWriters.count(t)) { w = t; hot = true; break; }
                        if (a.unknownRateWriters.count(t)) continue;
                        if (unresolvedThreads.count(t) || hasUnknownIndex(t)) continue;
                        if (!once) { w = t; once = true; }
                    }
                    if (!hot && !once) continue;
                    const uint64_t sa = sizeOf(a, w, hot), sb = sizeOf(b, w, true);
                    // Bytes the two accesses have in common are the same
                    // data, not a layout question.
                    if (overlap(oa, sa, elemSize + ob, sb)) continue;
                    unsigned positions = 0, sharing = 0;
                    for (uint64_t shift = 0; shift < line; shift += std::min(startStep, line)) {
                        ++positions;
                        sharing += sharesAt(oa, sa, elemSize + ob, sb, shift);
                    }
                    if (!sharing) continue;
                    if (!hot) { ++coldPairs; continue; }
                    std::string where;
                    if (sharing != positions) where = positionsText(sharing, positions);
                    ++definite;
                    Operation *at = pick(a, w);
                    auto d = at->emitRemark()
                        << "wtc: false sharing in " << label << " across elements: '" << a.name
                        << "' (offset " << oa << ") of one element and '" << b.name << "' (offset "
                        << ob << ") of the next are touched by the two threads the spawn loop "
                           "handed them to and share one " << line << "-byte line" << where
                        << "; padding each element to a multiple of a line and aligning the "
                           "array to a line removes it";
                    if (Operation *o = pick(b, w)) d.attachNote(o->getLoc())
                        << "wtc: the next element's '" << b.name << "' is touched here";
                }
            }
        }
    }

    // Printed whenever a field access was seen, so that "nothing to report"
    // is a line in the output rather than an absence a test cannot tell from
    // the pass not having run.
    // One operation may make two accesses (memcpy), each placed its own
    // way; it is counted once, under the least that is known of it.
    auto countExcept = [](const llvm::DenseSet<Operation *> &set,
                          std::initializer_list<const llvm::DenseSet<Operation *> *> before) {
        size_t n = 0;
        for (Operation *op : set) {
            bool earlier = false;
            for (const auto *b : before) earlier |= b->count(op) > 0;
            n += !earlier;
        }
        return n;
    };
    const size_t unknownIndexCount = countExcept(unknownIndexOps, {&unresolvedOps});
    const size_t stackLocalCount = countExcept(stackLocalOps, {&unresolvedOps, &unknownIndexOps});
    const size_t threadLocalCount = countExcept(threadLocalOps, {&unresolvedOps, &unknownIndexOps, &stackLocalOps});
    if (!byRoot.empty() || !unresolvedOps.empty() || !stackLocalOps.empty() ||
        !threadLocalOps.empty() || !unknownIndexOps.empty())
        module.emitRemark() << "wtc: false sharing: " << definite << " definite, " << possible
                            << " possible, " << coldPairs
                            << " cold (different threads, but one side touches the line once), "
                            << unresolvedOps.size() << " field accesses on objects that could not "
                            << "be identified, " << stackLocalCount
                            << " on stack slots that never escape, " << threadLocalCount
                            << " on thread-local objects, " << unknownIndexCount
                            << " at array elements the analysis cannot tell apart; " << walkInstances
                            << " function instances; " << unplacedPairs
                            << " of the possible pairs would be cold but for unplaced accesses";
}

} // namespace

namespace mlir::wtc {
    void registerWTCFalseSharingDiagnosePass() {
        PassRegistration<WTCFalseSharingDiagnosePass>();
    }
}
