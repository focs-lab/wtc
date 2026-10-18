#pragma once

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "llvm/ADT/StringRef.h"

#include <optional>

namespace mlir::wtc {
    enum class SemanticOpKind {
        Unknown,
        ThreadSpawn
    };

    // The ownership verdict, written by --queue-ownership-analysis onto each
    // queue's root (its cir.global or cir.alloca) and onto each push and pop:
    // one of "SPSC", "MPSC", "SPMC", "MPMC" or "UNKNOWN". An op that can
    // reach several queues carries the weakest of their verdicts.
    inline constexpr StringRef kQueueKindAttrName = "wtc.queue_kind";

    bool isWTCAnnotatedCall(cir::CallOp op, StringRef annotation);

    bool funcHasAnnotation(cir::FuncOp funcOp, StringRef annotation);

    // The element type of a queue whose push is pushFunc: the pointee of the
    // reference it takes the value by.
    Type deriveQueueElementTypeFromPush(cir::FuncOp pushFunc);

    SemanticOpKind getSemanticKind(cir::FuncOp funcOp);

    // The argument through which funcOp returns its value indirectly, if it
    // does. Whether it does depends on the target: on x86-64 a non-trivial
    // record is returned through a hidden first argument, which ClangIR
    // marks llvm.sret; on AArch64 the same function returns it directly.
    std::optional<unsigned> sretArgIndex(cir::FuncOp funcOp);

    // Gives a rebuilt call the argument attributes of its callee, sret
    // among them. A call that drops sret no longer matches the ABI of the
    // function it calls.
    void copyArgAttrsFromCallee(cir::CallOp call, cir::FuncOp callee);

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