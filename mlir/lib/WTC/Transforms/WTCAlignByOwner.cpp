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

// Two objects owned by different threads, landing in one cache line.
//
// This is the half of the problem the language cannot express. `alignas`
// lives inside a type, so it can push two fields of one record apart and can
// say nothing at all about two separate objects: where they land is the
// linker's decision, taken after every translation unit has had its say.
// A profiler cannot close the gap either, because it reports an address and
// an address does not name the allocation that produced it.
//
// It is also, measured across seventeen codebases, about half of what
// people write by hand: 795 of 1,623 cache-line annotations separate whole
// objects against 828 that separate fields (docs/corpus.md). And it is the cheap
// half to perform, which took a wrong turn to find out. A record's member
// alignment is not a property of the member in CIR -- `cir.struct` carries
// members, name, packed and member kinds, and no alignment -- so aligning a
// field means inserting a padding member, which means a new type and every
// cir.get_member index after it renumbered, exactly the work reordering
// needs. A *global's* alignment is an attribute of the operation:
//
//     cir.global external @s = #cir.zero : !rec_Stats {alignment = 8 : i64}
//
// so raising it is one attribute, no type rebuilt, no index moved. Raising
// an alignment is safe by construction: the linker takes the maximum across
// translation units, nothing inside the object moves, and no offset any
// other code computed changes. The cost is bytes, and the pass counts them.
//
// What it will not do. It never lowers an alignment. It refuses a constant,
// which nobody writes. It refuses an object touched only while the program
// starts, because that cannot contend with threads which do not exist yet.
// And it acts only on a *pair*: a global repeatedly written by one thread
// and another repeatedly touched by a different one. A lone hot global has
// nothing to be separated from, and padding it would buy nothing.

namespace {

struct RootInfo {
    llvm::SmallSetVector<unsigned, 4> hotWriters, hotReaders, certain;
    Operation *firstHotWrite = nullptr;
    Operation *firstHotAccess = nullptr;
};

class WTCAlignByOwnerPass
    : public PassWrapper<WTCAlignByOwnerPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCAlignByOwnerPass)

    WTCAlignByOwnerPass() = default;
    WTCAlignByOwnerPass(const WTCAlignByOwnerPass &other) : PassWrapper(other) {}

    StringRef getArgument() const final { return "wtc-align-by-owner"; }
    StringRef getDescription() const final {
        return "Raise the alignment of globals that different threads would otherwise share a "
               "cache line with; never lowers one, and counts the bytes it costs";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    Option<unsigned> lineSize{
        *this, "cache-line",
        llvm::cl::desc("Bytes in a cache line on the target (default 64)"),
        llvm::cl::init(64)};
    Option<unsigned> maxInstances{
        *this, "max-instances",
        llvm::cl::desc("Give up past this many function instances (default 250000)"),
        llvm::cl::init(ThreadInstanceWalk::kInstanceCap)};
    Option<bool> dryRun{
        *this, "dry-run",
        llvm::cl::desc("Report what would be aligned and change nothing"),
        llvm::cl::init(false)};

    void runOnOperation() override;

private:
    // Why a global's alignment must stay as it is, or null if it may move.
    // Raising it is only safe where this module owns the object outright:
    // on a declaration it promises an alignment the defining module never
    // gave, which LLVM is free to rely on; a weak or link-once copy may be
    // dropped by the linker for another module's copy; a section's objects
    // are walked by __start_/__stop_ tables that padding breaks; a
    // thread-local object is already private to its thread; an incomplete
    // type has no size to pad.
    static const char *cannotMove(cir::GlobalOp g) {
        if (g.isDeclaration()) return "it is only declared here";
        if (g.getSection()) return "it sits in a named section";
        if (g.getTlsModelAttr()) return "it is thread-local";
        switch (g.getLinkage()) {
        case cir::GlobalLinkageKind::ExternalLinkage:
        case cir::GlobalLinkageKind::InternalLinkage:
        case cir::GlobalLinkageKind::PrivateLinkage:
            break;
        default:
            return "its linkage lets the linker pick another module's copy";
        }
        if (auto rec = dyn_cast<cir::RecordType>(g.getSymType()))
            if (rec.isIncomplete()) return "its type is incomplete";
        return nullptr;
    }

    // The address an access works on, stripped of member and cast steps, so
    // a field access and a whole-object access both name the object.
    static Value objectAddress(Value addr) {
        Value cur = traceToLocalRoot(addr);
        unsigned guard = 0;
        while (guard++ < 64) {
            if (auto gm = cur.getDefiningOp<cir::GetMemberOp>()) {
                cur = traceToLocalRoot(gm.getAddr());
                continue;
            }
            return cur;
        }
        return Value();
    }
};

void WTCAlignByOwnerPass::runOnOperation() {
    ModuleOp module = getOperation();
    const DataLayout dl(module);
    const uint64_t line = lineSize;

    llvm::MapVector<Operation *, RootInfo> globals;

    ThreadInstanceWalk *self = nullptr;
    auto touchesMemory = [](Operation *op) {
        llvm::SmallVector<MemoryAccess, 2> accesses;
        memoryAccesses(op, accesses);
        return !accesses.empty();
    };
    auto relevant = [&](cir::FuncOp f) { return self->reachesOp(f, touchesMemory); };
    ThreadInstanceWalk walk(module, relevant, maxInstances);
    self = &walk;

    walk.run(
        [&](Instance &inst) {
            inst.func.walk([&](Operation *op) {
                llvm::SmallVector<MemoryAccess, 2> accesses;
                memoryAccesses(op, accesses);
                for (const MemoryAccess &ma : accesses) {
                const bool isWrite = ma.isWrite;
                Value obj = objectAddress(ma.addr);
                if (!obj) continue;
                Operation *root = walk.resolveRoot(obj, &inst);
                // Only globals: a stack slot is a different object in every
                // invocation, and a heap object's placement is the
                // allocator's, not ours.
                if (!root || !isa<cir::GlobalOp>(root)) continue;

                // An access while the program starts cannot contend with a
                // thread that does not exist yet; one that happens once
                // moves the line once.
                const bool repeated = !walk.inStartupPhase(op, inst) &&
                                      (inst.threadMult == Mult::Many ||
                                       inst.execMult == Mult::Many ||
                                       walk.siteMult(op, inst.func) == Mult::Many);
                if (!repeated) continue;

                RootInfo &ri = globals[root];
                if (!inst.speculative) ri.certain.insert(inst.thread);
                if (!ri.firstHotAccess) ri.firstHotAccess = op;
                if (isWrite) {
                    ri.hotWriters.insert(inst.thread);
                    if (!ri.firstHotWrite) ri.firstHotWrite = op;
                } else {
                    ri.hotReaders.insert(inst.thread);
                }
                }
            });
        },
        [](cir::FuncOp) { /* recursion: the map would be cyclic; skip the subtree */ });

    if (walk.capExceeded()) {
        module.emitRemark() << "wtc: gave up after " << walk.cap()
                            << " function instances; nothing is realigned in this module";
        return;
    }

    // A pair is a global repeatedly written by one accounted-for thread and
    // another repeatedly touched by a different one. Both ends are marked:
    // aligning only one leaves the other free to land in its line's tail.
    llvm::SmallVector<Operation *, 16> order;
    for (auto &kv : globals) order.push_back(kv.first);

    llvm::SmallSetVector<Operation *, 16> candidates;
    // Every partner, not the first one found: whether raising a global
    // separates it from anything depends on whether some partner can be
    // separated too, and an earlier version, remembering only the first,
    // raised a global whose remembered partner was fine while the one it
    // was paired with in the remark could not move.
    llvm::DenseMap<Operation *, llvm::SmallSetVector<Operation *, 4>> partners;
    for (size_t i = 0; i < order.size(); ++i) {
        for (size_t j = i + 1; j < order.size(); ++j) {
            const RootInfo &a = globals[order[i]];
            const RootInfo &b = globals[order[j]];
            auto contends = [](const RootInfo &x, const RootInfo &y) {
                for (unsigned w : x.hotWriters) {
                    if (!x.certain.count(w)) continue;
                    for (const auto &set : {y.hotWriters, y.hotReaders})
                        for (unsigned u : set)
                            if (u != w && y.certain.count(u)) return true;
                }
                return false;
            };
            if (!contends(a, b) && !contends(b, a)) continue;
            candidates.insert(order[i]);
            candidates.insert(order[j]);
            partners[order[i]].insert(order[j]);
            partners[order[j]].insert(order[i]);
        }
    }

    unsigned raised = 0, refusedConst = 0, refusedAligned = 0, refusedPartner = 0,
             refusedFixed = 0;
    uint64_t bytesAdded = 0;

    for (Operation *root : candidates) {
        auto g = cast<cir::GlobalOp>(root);
        const std::string label = ("@" + g.getSymName()).str();
        if (g.getConstant()) { ++refusedConst; continue; }   // nobody writes it
        if (const char *why = cannotMove(g)) {
            ++refusedFixed;
            g->emitRemark() << "wtc: left " << label << " where it is: " << why;
            continue;
        }
        const uint64_t have = g.getAlignment().value_or(dl.getTypeABIAlignment(g.getSymType()));
        if (have >= line) { ++refusedAligned; continue; }
        // Raising one end of a pair while the other stays put buys nothing:
        // an 8-byte object on its own line still has 56 bytes of tail for
        // the partner to land in. So a global is raised only if some
        // partner will also start a line of its own -- raised here, or
        // already aligned to one -- and the remark names that partner. A
        // constant is never raised, and neither is a global that cannot move.
        cir::GlobalOp other;
        for (Operation *p : partners.lookup(root)) {
            auto pg = cast<cir::GlobalOp>(p);
            if (pg.getConstant() || cannotMove(pg)) continue;
            other = pg;
            break;
        }
        if (!other) { ++refusedPartner; continue; }

        // What the alignment costs: at worst the padding needed to reach the
        // next line boundary. Reported so the price is never invisible.
        const uint64_t size = dl.getTypeSize(g.getSymType());
        bytesAdded += line > have ? line - have : 0;

        if (!dryRun) g.setAlignment(line);
        ++raised;
        auto d = g->emitRemark()
                 << "wtc: " << (dryRun ? "would raise" : "raised") << " the alignment of "
                 << label << " from " << have << " to " << line
                 << " bytes: a different thread repeatedly touches " << "@"
                 << other.getSymName() << ", and nothing in the language keeps two objects "
                    "off one cache line (size " << size << " bytes)";
        if (Operation *at = globals[root].firstHotWrite ? globals[root].firstHotWrite
                                                        : globals[root].firstHotAccess)
            d.attachNote(at->getLoc()) << "wtc: this thread touches " << label << " here";
    }

    // Printed whenever a global was touched repeatedly after start-up, so
    // that "nothing to align" is a line a test can check rather than an
    // absence it cannot tell from the pass not having run.
    if (!globals.empty())
        module.emitRemark() << "wtc: align by owner: " << raised << " global"
                            << (raised == 1 ? "" : "s")
                            << (dryRun ? " would be raised to " : " raised to ") << line
                            << " bytes, at most " << bytesAdded << " bytes of padding; refused "
                            << refusedConst << " constant, " << refusedAligned
                            << " already aligned, " << refusedPartner
                            << " whose partner could not be raised, " << refusedFixed
                            << " that cannot move";

    if (dryRun) markAllAnalysesPreserved();
}

} // namespace

namespace mlir::wtc {
    void registerWTCAlignByOwnerPass() {
        PassRegistration<WTCAlignByOwnerPass>();
    }
}
