#pragma once

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "llvm/ADT/StringRef.h"

namespace mlir::wtc {

// The shim functions, recognised by the annotation strings the headers in
// shim/ carry -- and the handful of standard entry points that mean the
// same thing in code nobody wrote for us. A program that starts its threads
// with pthread_create says exactly what wtc::spawn says; refusing to read
// it would mean the analysis knows a program has threads but not where they
// begin, which is what an amalgamated memcached showed: with no spawn in
// sight, the start-up phase collapses and the option parsing pairs against
// every worker.
enum class SemanticOpKind {
    Unknown,
    ThreadSpawn,
    QueueConstruction,
    QueuePush,
    QueueTryPop,
    MutexLock,
    MutexUnlock,
};

// The callee of a direct call, or null for an indirect call or an
// unresolvable symbol.
cir::FuncOp lookupCallee(cir::CallOp op);

bool hasAnnotation(cir::FuncOp funcOp, StringRef annotation);

bool isWTCAnnotatedCall(cir::CallOp op, StringRef annotation);

SemanticOpKind getSemanticKind(cir::FuncOp funcOp);

// Whether operand `idx` of the call is the hidden return slot the calling
// convention inserts for a non-trivial return type. ClangIR marks it with
// the `llvm.sret` argument attribute on the call site and on the callee;
// both are consulted so that hand-written IR may omit the call-site copy.
bool hasSretArg(cir::CallOp op, unsigned idx);

// 1 if operand 0 is a return slot, else 0: the index of the first operand
// that is an argument the source program wrote.
unsigned firstRealOperand(cir::CallOp op);

// For a call that starts a thread: the operand holding the entry function,
// and the operand holding the first argument passed to it. The two spellings
// disagree -- wtc::spawn(entry, args...) puts the entry first, while
// pthread_create(&tid, attr, entry, arg) puts it third -- so every caller
// asks rather than assuming.
unsigned spawnEntryOperand(cir::CallOp op);
unsigned spawnFirstArgOperand(cir::CallOp op);

// A function-typed operand (a thread entry point, say) traced through casts
// to the cir.get_global that names it, resolved to the function. Null if
// the trail does not end at a named function.
cir::FuncOp resolveFunctionOperand(Value v);

// Walks backward from v through pass-through casts -- cir.cast and the
// single-operand form of builtin.unrealized_conversion_cast -- to whatever
// value is no longer a cast. Purely local, single-value tracing.
Value traceToLocalRoot(Value v);

// traceToLocalRoot plus one more rule, the one real compiler output needs:
// a load from a stack slot that is written exactly once, in the same block
// as the slot and before every load, continues from the stored value. That
// is the shape every parameter and local pointer takes at -O0 and -O1:
//
//     %p = cir.alloca "p"
//     cir.store %arg, %p
//     ...
//     %v = cir.load %p
//
// Anything else (two stores, the slot's address escaping) stops the walk,
// and the caller treats the result as unresolved, which is the safe answer.
Value traceToRoot(Value v);

}
