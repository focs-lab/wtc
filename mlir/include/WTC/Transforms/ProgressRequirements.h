#pragma once

// What progress a function is held to, and why (docs/progress.md,
// "Requirements").
//
// A requirement is declared on a function by an annotation of
// shim/progress.hpp:
//
//   WTC_LOCK_FREE              nothing it reaches blocks;
//   WTC_WAIT_FREE              that, and every path is wait-free: no retry,
//                              no wait for data;
//   WTC_WAIT_FREE_WITHIN(k)    that, and its worst case is a number of steps
//                              no more than k under the counts proved;
//   WTC_PROGRESS_CRITICAL      wait-free, with a worst case (not only an
//                              amortised bound) that is a polynomial in the
//                              counts, and no first-use effects.

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mlir::wtc {

struct Requirement {
    // Strongest last.
    enum class Kind : uint8_t { lock_free, wait_free, critical };
    Kind kind = Kind::lock_free;
    // WTC_WAIT_FREE_WITHIN(k): the most steps the worst case may take.
    std::optional<uint64_t> within;

    // "lock-free", "wait-free", "wait-free within 100 steps",
    // "progress-critical".
    std::string describe() const;
    // Whether this requirement is at least as strong as another.
    bool covers(const Requirement &o) const;
};

// The requirement f declares, the strongest of its annotations; nothing
// when it declares none.
std::optional<Requirement> declaredRequirement(cir::FuncOp f);

} // namespace mlir::wtc
