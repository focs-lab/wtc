#pragma once

#include "clang/CIR/Dialect/IR/CIRDialect.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <optional>

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
    QueueBatchBegin,
    QueuePushDeferred,
    QueueBatchEnd,
    MutexLock,
    MutexUnlock,
    MutexBatchBegin,
    MutexBatchStep,
    MutexBatchEnd,
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

// One address an operation reads or writes through: a plain load or store,
// an atomic read-modify-write, exchange or compare-exchange, a bit-field
// access, or either end of a cir.copy. `type` is what is read or written
// there, which sizes the access. A review found both layout passes
// accepting loads and stores only, so the textbook pair of atomic counters
// -- two threads, two fetch-adds, one line -- was invisible to them.
struct MemoryAccess {
    Value addr;
    Type type;
    bool isWrite;
};

// Every address `op` touches, in `out`; nothing for an operation that
// touches no memory. A read-modify-write counts as a write: it is the
// write that moves the line.
void memoryAccesses(Operation *op, llvm::SmallVectorImpl<MemoryAccess> &out);

// Whether a call allocates an object the program then owns: malloc,
// calloc, aligned_alloc, memalign, operator new; posix_memalign is named
// too, though its buffer leaves by an out-parameter and is not followed.
// realloc is not one: its result replaces an older object, and the slot
// that receives it has two stores anyway. Such a call is the root of the
// object it returns, the way a cir.global or a cir.alloca is: an identity
// for "the object allocated here", standing for every object the site
// allocates when the site runs more than once.
bool isAllocator(cir::FuncOp f);

// The alignment the allocator guarantees for a site's result, when the
// call says it: aligned_alloc, memalign and posix_memalign with a constant
// alignment operand. Nothing for malloc, calloc and new, whose guarantee is
// the platform's.
std::optional<uint64_t> allocationAlignment(cir::CallOp call);

// The allocator call a call hands back: the call itself when its callee is
// an allocator, else the one allocator call every return of the callee's
// body hands back, through further such wrappers (Phoenix's CALLOC, any
// checked malloc, with or without a null on its failure path). Null when
// the callee is neither, returns from two sites, or returns anything else.
cir::CallOp allocationBehind(cir::CallOp call);
// Every allocation a call may hand back, when each of its returns hands
// back an allocation or a null: one site for a checked wrapper, several for
// a constructor that picks between allocators; empty when some return
// hands back anything else.
llvm::SmallVector<cir::CallOp, 4> allocationsBehind(cir::CallOp call);

// Walks backward from v through the casts under which it still names the
// same object -- bitcast, array-to-pointer decay, and the single-operand
// form of builtin.unrealized_conversion_cast -- to whatever value is no
// longer such a cast. A pointer made from an integer stops the walk.
// Purely local, single-value tracing.
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

// Whether a stack slot's address goes anywhere but its own loads and
// stores: stored as a value, passed to a call, or used by anything but a
// cast, member or element step that itself goes nowhere else. Out of
// budget counts as escaping.
bool addressEscapes(cir::AllocaOp slot);

// The functions an indirect call may reach: the one a function's address
// names, or every function handed in for the parameter the call goes
// through, at every direct call of the enclosing function. Empty when any
// of that is unknown, or the enclosing function's own address is taken.
llvm::SmallVector<cir::FuncOp, 4> indirectCallees(cir::CallOp call);

// Every call to a shim queue constructor, keyed by the object it constructs:
// a cir.global, or a cir.alloca, reached from the constructor's `this`
// through traceToRoot. Constructors whose object is anything else are left
// out, so a queue missing from the map has no constructor this can name.
llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>>
collectQueueConstructors(ModuleOp module);

// The topology tag a queue's one constructor passes, if it is a constant;
// nullopt when there is not exactly one constructor of the two-operand
// shape, or its tag is not a constant.
std::optional<int64_t> constructorTag(llvm::ArrayRef<cir::CallOp> ctors);

}
