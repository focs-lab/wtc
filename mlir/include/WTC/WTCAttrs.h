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
// and the specialisation pass reads. The root is the cir.global or the
// cir.alloca that is the queue, or, for a queue that is a member of a
// struct or a heap object, the one constructor call that builds it.
inline constexpr llvm::StringLiteral kTopologyAttrName = "wtc.topology";
// On a constructor call that is a queue's root, the name the analysis gives
// that queue in its remarks ("@c.q"), for the passes after it.
inline constexpr llvm::StringLiteral kQueueNameAttrName = "wtc.queue_name";
// Left by the specialisation pass on a root it rewrote.
inline constexpr llvm::StringLiteral kSpecializedAttrName = "wtc.specialized";
// The number the ownership analysis gives each queue, on its root, in
// discovery order; an operation that can only act on that
// queue carries the same number in its queue_id.
inline constexpr llvm::StringLiteral kQueueIdAttrName = "wtc.queue_id";
// Left by the borrow analysis on an spsc queue root whose every pop only
// borrows its element (WTCBorrowAnalysis.cpp).
inline constexpr llvm::StringLiteral kBorrowedAttrName = "wtc.borrowed";

} // namespace mlir::wtc

#endif // WTC_WTCATTRS_H
