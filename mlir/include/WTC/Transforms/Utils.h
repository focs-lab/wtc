#pragma once

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "llvm/ADT/StringRef.h"

namespace mlir::wtc {
    enum class SemanticOpKind {
        Unknown,
        ThreadSpawn
    };

    bool isWTCAnnotatedCall(cir::CallOp op, StringRef annotation);

    SemanticOpKind getSemanticKind(cir::FuncOp funcOp);

    // Walks backward from v through pass-through casts -- cir.cast and the
    // single-operand form of builtin.unrealized_conversion_cast -- to
    // whatever value is no longer a cast. That's either the result of a
    // "real" op (an alloca, a global, ...) or a block argument if the trail
    // runs off the edge of the current function; the caller distinguishes
    // the two with isa<BlockArgument>. Resolving further than a block
    // argument requires knowing which call/spawn site supplied it, which
    // this function -- being purely local, single-value tracing -- has no
    // visibility into.
    Value traceToLocalRoot(Value v);
}