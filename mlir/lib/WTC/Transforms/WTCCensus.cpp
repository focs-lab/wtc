#include "WTC/Transforms/Passes.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/StringRef.h"

#include <array>
#include <optional>

using namespace mlir;

// Counts, at run time, which threads touch each std::shared_ptr control
// block. libstdc++'s reference counts are inline member functions of
// _Sp_counted_base<_S_atomic>; once the optimiser has inlined them nothing
// is left to interpose, so the count has to be planted before that, in the
// CIR module, where each of them is still a function of its own. This pass
// puts a call
//
//     wtc_rc_note(control block, operation)
//
// at the entry of every such function the module defines. The operations
// are numbered as tools/census/threadcensus.c reads them:
//
//     0  _M_add_ref_copy          a shared_ptr copied
//     1  _M_release               a shared_ptr destroyed or reset
//     2  _M_add_ref_lock_nothrow  a weak_ptr locked
//     3  _M_weak_add_ref          a weak_ptr made
//     4  _M_weak_release          a weak_ptr destroyed
//     5  _M_destroy               the control block freed (the base's or an override)
//     6  the constructor          the control block made, both counts set to one
//
// The library behind the call is the census's preloaded one, so the
// declaration is weak and the program links without it; it must run with
// the library preloaded. A program that defines wtc_rc_note itself gets
// its own definition called instead, which is how the integration test
// checks the counts. Only the atomic
// policy is instrumented: _S_single counts belong to a program built
// without threads, and _S_mutex to targets without atomics. What the
// module only declares (an extern template, a function from another
// translation unit) is not seen, and the pass says how many it found.

namespace {

constexpr llvm::StringLiteral kAtomicBase = "_ZNSt16_Sp_counted_baseILN9__gnu_cxx12_Lock_policyE2EE";

struct Member {
    llvm::StringLiteral suffix;
    int op;
};
constexpr std::array<Member, 8> kMembers{{
    {"15_M_add_ref_copyEv", 0},
    {"10_M_releaseEv", 1},
    {"23_M_add_ref_lock_nothrowEv", 2},
    {"15_M_weak_add_refEv", 3},
    {"15_M_weak_releaseEv", 4},
    {"10_M_destroyEv", 5},  // `delete this`, for the blocks that do not override it
    {"C2Ev", 6},            // the base constructor, which every block's runs
    {"C1Ev", 6},
}};

// The operation a function stands for, if it is one of the counted ones.
std::optional<int> operationOf(llvm::StringRef name) {
    if (name.consume_front(kAtomicBase)) {
        for (const Member &m : kMembers)
            if (name == m.suffix) return m.op;
        return std::nullopt;
    }
    // _Sp_counted_deleter and _Sp_counted_ptr_inplace override _M_destroy;
    // the base is their first and only base, so `this` is the block's
    // address in either.
    if (name.starts_with("_ZNSt") && name.contains("_Sp_counted_") && name.ends_with("10_M_destroyEv") &&
        name.contains("Lock_policyE2E"))
        return 5;
    return std::nullopt;
}

struct WTCCensusPass : public PassWrapper<WTCCensusPass, OperationPass<ModuleOp>> {
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCCensusPass)

    StringRef getArgument() const final { return "wtc-census"; }
    StringRef getDescription() const final {
        return "Call wtc_rc_note(control block, operation) at the entry of libstdc++'s shared_ptr "
               "reference-count functions, for the thread census (tools/census)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        ModuleOp module = getOperation();
        MLIRContext *ctx = &getContext();
        auto voidPtr = cir::PointerType::get(cir::VoidType::get(ctx));
        auto s32 = cir::IntType::get(ctx, 32, /*isSigned=*/true);

        llvm::SmallVector<std::pair<cir::FuncOp, int>, 16> targets;
        module.walk([&](cir::FuncOp f) {
            if (f.getBody().empty()) return;
            if (auto op = operationOf(f.getSymName()); op && f.getNumArguments() >= 1 &&
                                                       isa<cir::PointerType>(f.getArgument(0).getType()))
                targets.push_back({f, *op});
        });

        std::array<unsigned, 7> counts{};
        if (!targets.empty()) {
            auto note = module.lookupSymbol<cir::FuncOp>("wtc_rc_note");
            if (!note) {
                OpBuilder b = OpBuilder::atBlockEnd(module.getBody());
                // Weak: the census library defines it when preloaded; the
                // program is linked without it.
                note = cir::FuncOp::create(b, module.getLoc(), "wtc_rc_note",
                                           cir::FuncType::get({voidPtr, s32}, cir::VoidType::get(ctx)),
                                           cir::GlobalLinkageKind::ExternalWeakLinkage);
                SymbolTable::setSymbolVisibility(note, SymbolTable::Visibility::Private);
            }
            auto callee = FlatSymbolRefAttr::get(ctx, note.getSymName());
            for (auto [f, op] : targets) {
                OpBuilder b = OpBuilder::atBlockBegin(&f.getBody().front());
                Location loc = f.getLoc();
                Value self = f.getArgument(0);
                Value ctrl = cir::CastOp::create(b, loc, voidPtr, cir::CastKind::bitcast, self);
                Value code = cir::ConstantOp::create(b, loc, s32, cir::IntAttr::get(s32, op));
                cir::CallOp::create(b, loc, callee, Type{}, ValueRange{ctrl, code});
                ++counts[op];
            }
        }
        module.emitRemark() << "wtc-census: reference-count functions instrumented: " << targets.size()
                            << " (copy " << counts[0] << ", release " << counts[1] << ", lock "
                            << counts[2] << ", weak copy " << counts[3] << ", weak release " << counts[4]
                            << ", destroy " << counts[5] << ", make " << counts[6] << ")";
    }
};

} // namespace

namespace mlir::wtc {
void registerWTCCensusPass() { PassRegistration<WTCCensusPass>(); }
} // namespace mlir::wtc
