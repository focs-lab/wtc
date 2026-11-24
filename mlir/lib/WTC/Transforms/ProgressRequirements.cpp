#include "WTC/Transforms/ProgressRequirements.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"

namespace mlir::wtc {

std::string Requirement::describe() const {
    switch (kind) {
    case Kind::lock_free: return "lock-free";
    case Kind::wait_free:
        return within ? "wait-free within " + std::to_string(*within) + " steps" : "wait-free";
    case Kind::critical: return within ? "progress-critical within " + std::to_string(*within) + " steps" : "progress-critical";
    }
    return "lock-free";
}

bool Requirement::covers(const Requirement &o) const {
    if (kind < o.kind) return false;
    if (o.within && (!within || *within > *o.within)) return false;
    return true;
}

// ClangIR does not carry an annotation's arguments, so the bound of
// WTC_WAIT_FREE_WITHIN(k) is spelled in its name:
// "wtc_progress_wait_free_within_<k>".
std::optional<Requirement> declaredRequirement(cir::FuncOp f) {
    auto annotations = f.getAnnotationsAttr();
    if (!annotations) return std::nullopt;
    std::optional<Requirement> out;
    auto take = [&](Requirement r) {
        if (!out) {
            out = r;
            return;
        }
        if (r.kind > out->kind) out->kind = r.kind;
        if (r.within && (!out->within || *r.within < *out->within)) out->within = r.within;
    };
    for (Attribute a : annotations) {
        auto ann = dyn_cast<cir::AnnotationAttr>(a);
        if (!ann) continue;
        llvm::StringRef name = ann.getName().getValue();
        if (name == "wtc_progress_lock_free") take({Requirement::Kind::lock_free, std::nullopt});
        else if (name == "wtc_progress_wait_free") take({Requirement::Kind::wait_free, std::nullopt});
        else if (name == "wtc_progress_critical") take({Requirement::Kind::critical, std::nullopt});
        else if (name.consume_front("wtc_progress_wait_free_within_")) {
            uint64_t k;
            // A bound that does not read as a number promises nothing a
            // check could hold it to: zero steps, which every path fails.
            if (name.getAsInteger(10, k)) k = 0;
            take({Requirement::Kind::wait_free, k});
        }
    }
    return out;
}

} // namespace mlir::wtc
