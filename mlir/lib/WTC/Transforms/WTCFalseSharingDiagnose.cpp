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
#include "llvm/ADT/SmallSet.h"

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
    // The widest access each thread makes at this offset. A pair is judged
    // on the two accesses that make it, not on the widest access anyone
    // made: a start-up snapshot of a whole record once hid every pair of
    // its members, because the record's size was taken for the member's.
    llvm::DenseMap<unsigned, uint64_t> sizeOf;
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

    void runOnOperation() override;

private:
    ModuleOp module;
    llvm::MapVector<Operation *, FieldMap> byRoot;
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

    // Walks the chain of cir.get_member back to whatever the record sits in,
    // accumulating the byte offset. A member of a member contributes both
    // offsets, so a field nested two deep is placed correctly in its line.
    // Returns the base value, or null if the address is not a member access.
    Value resolveMember(Value addr, const DataLayout &dl, uint64_t &offset,
                        std::string &path) const {
        Value cur = traceToLocalRoot(addr);
        auto gm = cur.getDefiningOp<cir::GetMemberOp>();
        if (!gm) return Value();
        offset = 0;
        llvm::SmallVector<StringRef, 4> parts;
        unsigned guard = 0;
        while (gm && guard++ < 64) {
            auto ptrTy = dyn_cast<cir::PointerType>(gm.getAddr().getType());
            if (!ptrTy) return Value();
            auto rec = dyn_cast<cir::RecordType>(ptrTy.getPointee());
            if (!rec || rec.isIncomplete()) return Value();
            offset += rec.getElementOffset(dl, gm.getIndex());
            parts.push_back(gm.getName());
            Value base = traceToLocalRoot(gm.getAddr());
            auto outer = base.getDefiningOp<cir::GetMemberOp>();
            if (!outer) {
                for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
                    if (!path.empty()) path += ".";
                    path += it->str();
                }
                return base;
            }
            gm = outer;
        }
        return Value();
    }

    void report(const ThreadInstanceWalk &walk);

    void noteUnresolved(Operation *op, const Instance &inst) {
        unresolvedOps.insert(op);
        unresolvedThreads.insert(inst.thread);
    }

    // What an address is, read backwards through casts: a member chain, an
    // element of an array (indexed, so not yet placed), the whole of an
    // object reached through a cast of its address, or none of these.
    enum class Shape { None, Member, Array, Whole };
    static Shape shapeOf(Value addr) {
        Value v = traceToLocalRoot(addr);
        bool array = false;
        for (unsigned guard = 0; guard < 64; ++guard) {
            if (v.getDefiningOp<cir::GetMemberOp>()) return array ? Shape::Array : Shape::Member;
            if (auto ps = v.getDefiningOp<cir::PtrStrideOp>()) { array = true; v = traceToLocalRoot(ps.getBase()); continue; }
            if (auto ge = v.getDefiningOp<cir::GetElementOp>()) { array = true; v = traceToLocalRoot(ge.getBase()); continue; }
            break;
        }
        if (array) return Shape::Array;
        Value t = traceToRoot(addr);
        if (t.getDefiningOp<cir::GetGlobalOp>() || t.getDefiningOp<cir::AllocaOp>())
            return Shape::Whole;
        return Shape::None;
    }

    static Type rootType(Operation *root) {
        if (auto g = dyn_cast<cir::GlobalOp>(root)) return g.getSymType();
        if (auto a = dyn_cast<cir::AllocaOp>(root)) return a.getAllocaType();
        return Type();
    }

    // Whether the data layout can size and align the object a root names.
    static bool hasLayout(Operation *root) {
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
                if (!seen.insert(u).second) continue;
                if (isa<cir::LoadOp>(u)) continue;                  // reads the slot
                if (auto st = dyn_cast<cir::StoreOp>(u)) {
                    // Storing *into* the slot is fine; storing the slot's own
                    // address somewhere publishes it.
                    if (st.getValue() == v) return true;
                    continue;
                }
                if (isa<cir::CastOp, cir::GetMemberOp>(u)) {
                    work.push_back(u->getResult(0));
                    continue;
                }
                return true;    // a call, a global initialiser, anything else
            }
        }
        return true;            // ran out of budget: assume the worst
    }
};

void WTCFalseSharingDiagnosePass::runOnOperation() {
    module = getOperation();
    byRoot.clear();
    multiInvocation.clear();
    unresolvedOps.clear();
    stackLocalOps.clear();
    unresolvedThreads.clear();
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
                const bool startup = walk.inStartupPhase(op, inst);
                const bool manyAnyway = (inst.threadMult == Mult::Many || inst.execMult == Mult::Many) &&
                                        !inst.execUnknown;
                bool siteKnown;
                const Mult site = walk.siteMult(op, inst.func, siteKnown);
                const bool repeated = !startup && (manyAnyway || (siteKnown && site == Mult::Many));
                const bool unknownRate = !startup && !repeated && (!siteKnown || inst.execUnknown);
                auto unplaced = [&] {
                    unresolvedOps.insert(op);
                    if (repeated || unknownRate) unresolvedThreads.insert(inst.thread);
                };

                uint64_t offset = 0;
                std::string path;
                Operation *root = nullptr;
                if (shape == Shape::Array) {
                    // An element of an array: which element is not yet read,
                    // so the access is counted as one the analysis could not
                    // place rather than dropped. Two per-thread counters in
                    // one array once produced no output at all.
                    unplaced();
                    return;
                }
                if (shape == Shape::Member) {
                    Value base = resolveMember(addr, dl, offset, path);
                    if (!base) { unplaced(); return; }
                    root = walk.resolveRoot(base, &inst);
                } else {
                    // The whole object through a cast of its address: `*(long
                    // *)&s`. Only a record is of interest; a scalar global has
                    // no second field to pair with.
                    root = walk.resolveRoot(addr, &inst);
                    if (root && !isa<cir::RecordType>(rootType(root))) return;
                    path = "<whole>";
                }
                if (!root) { unplaced(); return; }
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

                FieldInfo &fi = byRoot[root][offset];
                if (fi.name.empty()) fi.name = path;
                if (!fi.firstAccess) fi.firstAccess = op;
                const uint64_t size = dl.getTypeSize(valTy);
                fi.size = std::max<uint64_t>(fi.size, size);
                uint64_t &mine = fi.sizeOf[inst.thread];
                mine = std::max(mine, size);
                if (!inst.speculative) fi.certain.insert(inst.thread);
                if (isWrite) {
                    fi.writers.insert(inst.thread);
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
    for (auto &kv : byRoot)
        if (auto slot = dyn_cast<cir::AllocaOp>(kv.first))
            if (!walk.runsOnce(slot->getParentOfType<cir::FuncOp>()))
                multiInvocation.insert(kv.first);
    report(walk);
    markAllAnalysesPreserved();
}

void WTCFalseSharingDiagnosePass::report(const ThreadInstanceWalk &walk) {
    const uint64_t line = lineSize;
    const DataLayout dl(module);

    for (auto &[root, fields] : byRoot) {
        // Sorted by offset so the report reads like the struct does, and so
        // it does not depend on the order the walk happened to find them.
        llvm::SmallVector<uint64_t, 16> offsets;
        for (auto &kv : fields) offsets.push_back(kv.first);
        llvm::sort(offsets);

        std::string label;
        const bool onStack = isa<cir::AllocaOp>(root) && multiInvocation.count(root);
        if (auto g = dyn_cast<cir::GlobalOp>(root)) label = ("@" + g.getSymName()).str();
        else if (auto a = dyn_cast<cir::AllocaOp>(root)) label = ("%" + a.getName()).str();
        else label = "<object>";

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
        align = std::max<uint64_t>(align, 1);
        auto sharesAt = [&](uint64_t oa, uint64_t sa, uint64_t ob, uint64_t sb, uint64_t shift) {
            const uint64_t a0 = (oa + shift) / line, a1 = (oa + shift + std::max<uint64_t>(sa, 1) - 1) / line;
            const uint64_t b0 = (ob + shift) / line, b1 = (ob + shift + std::max<uint64_t>(sb, 1) - 1) / line;
            return a0 <= b1 && b0 <= a1;
        };

        // Geometry is judged per pair of accesses, with the sizes of the two
        // accesses that make the verdict. Overlapping bytes are the same data:
        // a store of a whole record against a store to one of its members is
        // true sharing, which no layout removes.
        auto sizeOf = [](const FieldInfo &f, unsigned t) -> uint64_t {
            auto it = f.sizeOf.find(t);
            return it == f.sizeOf.end() ? 1 : std::max<uint64_t>(it->second, 1);
        };
        auto overlap = [](uint64_t ox, uint64_t sx, uint64_t oy, uint64_t sy) {
            return ox < oy + sy && oy < ox + sx;
        };
        auto geometry = [&](uint64_t ox, uint64_t sx, uint64_t oy, uint64_t sy,
                            unsigned &positions, unsigned &sharing) {
            positions = sharing = 0;
            for (uint64_t shift = 0; shift < line; shift += std::min(align, line)) {
                ++positions;
                sharing += sharesAt(ox, sx, oy, sy, shift);
            }
            return sharing > 0;
        };

        for (size_t i = 0; i < offsets.size(); ++i) {
            for (size_t j = i + 1; j < offsets.size(); ++j) {
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
                auto lands = [&](const FieldInfo &x, uint64_t ox, unsigned w,
                                 const FieldInfo &y, uint64_t oy, unsigned u) {
                    const uint64_t sx = sizeOf(x, w), sy = sizeOf(y, u);
                    if (overlap(ox, sx, oy, sy)) return false;
                    return geometry(ox, sx, oy, sy, positions, sharing);
                };
                auto tryPair = [&](const FieldInfo &x, uint64_t ox, const FieldInfo &y, uint64_t oy,
                                   Operation *&wx, Operation *&wy) {
                    for (unsigned w : x.hotWriters) {
                        if (!x.certain.count(w)) continue;
                        for (const auto &set : {y.hotWriters, y.hotReaders})
                            for (unsigned u : set)
                                if (u != w && y.certain.count(u) && !walk.exclusive(w, u) &&
                                    lands(x, ox, w, y, oy, u)) {
                                    hit = true;
                                    if (!wx) { wx = pick(x, w); wy = pick(y, u); }
                                    return;
                                }
                    }
                };
                tryPair(a, oa, b, ob, whereA, whereB);
                if (!hit) tryPair(b, ob, a, oa, whereB, whereA);

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
                auto mayContend = [&](const FieldInfo &x, uint64_t ox, const FieldInfo &y, uint64_t oy,
                                      Operation *&wx, Operation *&wy) {
                    for (const auto &xs : {x.hotWriters, x.unknownRateWriters})
                        for (unsigned w : xs)
                            for (const auto &set : {y.hotWriters, y.hotReaders,
                                                    y.unknownRateWriters, y.unknownRateReaders})
                                for (unsigned u : set) {
                                    if (u == w && !x.manyHotWriters.count(w)) continue;
                                    if (!lands(x, ox, w, y, oy, u)) continue;
                                    wx = pick(x, w); wy = pick(y, u);
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
                const bool contend = !hit && (mayContend(a, oa, b, ob, whereA, whereB) ||
                                              mayContend(b, ob, a, oa, whereB, whereA));

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
                if (!hit && !contend) {
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
                                    if (!lands(x, ox, w, y, oy, u)) continue;
                                    if (unresolvedThreads.count(w)) {
                                        unplaced = true;
                                        wx = x.firstWrite; wy = pick(y, u);
                                    } else {
                                        any = true;
                                        if (!wx) { wx = x.firstWrite; wy = pick(y, u); }
                                    }
                                }
                        }
                        return any;
                    };
                    const bool ab = coldAgainst(a, oa, b, ob, whereA, whereB);
                    const bool ba = coldAgainst(b, ob, a, oa, whereB, whereA);
                    cold = (ab || ba) && !unplaced;
                }
                if (!hit && !contend && !cold && !unplaced) continue;   // never on one line

                const bool shareAll = sharing == positions;
                std::string where;
                if (!shareAll)
                    where = (" in " + llvm::Twine(sharing) + " of the " + llvm::Twine(positions) +
                             " positions an object aligned to " + llvm::Twine(align) +
                             " bytes may start at").str();

                if (hit && onStack) {
                    // The address was published, so two threads may be
                    // looking at it -- but they may equally be looking at two
                    // invocations, which are two objects. Not a claim.
                    ++possible;
                    Operation *at = a.firstWrite ? a.firstWrite : a.firstAccess;
                    at->emitRemark()
                        << "wtc: possible false sharing in " << label << ": '" << a.name
                        << "' and '" << b.name << "' share one " << line << "-byte line" << where
                        << ", but the object is a stack slot whose address escapes, "
                           "so two threads may be looking at two invocations rather than one";
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
                // On a stack slot, one thread writing both fields is one
                // invocation writing its own copy, and saying anything about
                // it is noise: it is what libstdc++'s own locals produced on
                // real output.
                if (contend && !onStack) {
                    ++possible;
                    auto d = whereA->emitRemark()
                        << "wtc: possible false sharing in " << label << ": '" << a.name
                        << "' and '" << b.name << "' share one " << line << "-byte line" << where
                        << " and are touched repeatedly, but " << why;
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
    }

    // Printed whenever a field access was seen, so that "nothing to report"
    // is a line in the output rather than an absence a test cannot tell from
    // the pass not having run.
    if (!byRoot.empty() || !unresolvedOps.empty() || !stackLocalOps.empty() ||
        !threadLocalOps.empty())
        module.emitRemark() << "wtc: false sharing: " << definite << " definite, " << possible
                            << " possible, " << coldPairs
                            << " cold (different threads, but one side touches the line once), "
                            << unresolvedOps.size() << " field accesses on objects that could not "
                            << "be identified, " << stackLocalOps.size()
                            << " on stack slots that never escape, " << threadLocalOps.size()
                            << " on thread-local objects; " << walkInstances
                            << " function instances; " << unplacedPairs
                            << " of the possible pairs would be cold but for unplaced accesses";
}

} // namespace

namespace mlir::wtc {
    void registerWTCFalseSharingDiagnosePass() {
        PassRegistration<WTCFalseSharingDiagnosePass>();
    }
}
