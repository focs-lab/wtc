#ifndef WTC_WTCATTRS_H
#define WTC_WTCATTRS_H

#include "mlir/IR/Attributes.h"
#include "mlir/IR/BuiltinAttributes.h"

#include "WTC/WTCDialect.h"

#include "WTC/WTCEnums.h.inc"

#define GET_ATTRDEF_CLASSES
#include "WTC/WTCAttrs.h.inc"

namespace mlir::wtc {

// The discardable attribute the analysis leaves on a queue's root operation
// (a cir.global or a cir.alloca) and the specialisation pass reads.
inline constexpr llvm::StringLiteral kTopologyAttrName = "wtc.topology";
// Left by the specialisation pass on a root it rewrote.
inline constexpr llvm::StringLiteral kSpecializedAttrName = "wtc.specialized";
// The number the ownership analysis gives each queue root (a global or an
// alloca), in discovery order; an operation that can only act on that
// queue carries the same number in its queue_id.
inline constexpr llvm::StringLiteral kQueueIdAttrName = "wtc.queue_id";
// Left by the borrow analysis on an spsc queue root whose every pop only
// borrows its element (WTCBorrowAnalysis.cpp).
inline constexpr llvm::StringLiteral kBorrowedAttrName = "wtc.borrowed";

} // namespace mlir::wtc

#endif // WTC_WTCATTRS_H
