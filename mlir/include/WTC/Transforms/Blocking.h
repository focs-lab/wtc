#pragma once

// May this operation wait for another thread, or block?
//
// Several passes ask that one question of the same code: the progress check
// (a function declared lock-free may reach nothing that blocks), and the
// two batching passes (a loop whose pushes are delayed, or whose critical
// sections run with the mutex held, may not wait for another thread). They
// used to answer it each with its own list of functions known to block, its
// own reading of inline assembly and its own walk of the call graph, and
// the answers drifted: the batching passes refused every indirect call the
// progress check could resolve, and the lock batching took realloc, which
// the progress check knew as the allocator, for a wait. The facts live
// here once (decisions/0004):
//
//   - a wtc operation answers for itself, through ProgressOpInterface: its
//     implementation, as recorded on it, can wait or may call the allocator;
//     and the element's special members it runs (a push's copy or move
//     constructor, a pop's move-assignment and destructor) are followed as
//     calls the operation makes, since they run inside it;
//   - a call to a function without a body here is read by its name: the
//     allocator, the threading library, other waits (a System V
//     semaphore, a file lock, a descriptor, a child, a signal, a DPDK
//     lcore), a library lock, input and output, sleeping, throwing, a
//     static-initialisation guard; a function that only touches the memory
//     it is handed blocks in no way; any other is opaque;
//   - a call to a function with a body is followed into it, unless that
//     body may be replaced at link time (weak, non-ODR linkonce, common,
//     extern_weak), which counts as no body; an indirect call into every
//     function it may reach (indirectCallees), or counted as unknown when
//     those are not known;
//   - inline assembly is not looked into, except a string made only of the
//     handful of instructions that cannot wait (onlyNonBlockingInstructions).
//
// What a pass does with a fact is its own: the progress check accepts no
// kind of blocking, the batching of critical sections the allocator, the
// batching of pushes the allocator and sleeping. A pass that refuses more
// than waiting (the batching passes refuse every atomic access, since a
// loop that synchronises may be waiting for an element not yet published)
// adds its own check, which is run at every operation the walk visits
// before the shared facts are.

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <optional>
#include <string>

namespace mlir::wtc {

enum class BlockingKind : unsigned {
    Allocates,      // the allocator, or a wtc operation whose implementation may call it
    Waits,          // waits for another thread: a lock, a condition, a join, a guard, a yield
    Threading,      // any other entry point of the threading library
    InputOutput,
    Sleeps,
    Throws,         // a throw: the runtime allocates the exception and unwinds
    MayThrow,       // a call to a function without a body here that is not nothrow
    Opaque,         // a function without a body here that is none of the above
    UnknownTarget,  // an indirect call whose targets are not known here
    Assembly,       // inline assembly not made only of instructions that cannot wait
    Refused,        // the asking pass's own check (BlockingQuery::Extra)
};

// A set of kinds, for what a pass accepts.
class BlockingKinds {
public:
    BlockingKinds() = default;
    BlockingKinds(std::initializer_list<BlockingKind> kinds) {
        for (BlockingKind k : kinds) bits |= 1u << static_cast<unsigned>(k);
    }
    bool contains(BlockingKind k) const { return bits & (1u << static_cast<unsigned>(k)); }

private:
    unsigned bits = 0;
};

// How a function without a body here may block, read from its name. `what`
// is the reason in words, naming the function ("waits in the threading
// library (pthread_mutex_lock)"); `threading` is set for every function of
// the threading library, whatever its kind.
// Nothing for a function that only touches the memory it is handed.
struct ExternalBlocking {
    BlockingKind kind;
    std::string what;
    bool threading = false;
};
std::optional<ExternalBlocking> externalBlocking(cir::FuncOp f);

// Accepting the allocator assumes it is a leaf: it waits for no thread of
// the program and runs none of the program's code. A program can break
// that by registering code the allocator calls: a DPDK memory event
// callback or allocation validator, run inside rte_malloc and rte_free, a
// new-handler, run inside operator new when memory runs short, or one of
// glibc's old allocation hooks. The name of the first such registration
// the module makes, or nothing when it makes none.
std::optional<std::string> allocatorCallbackRegistration(ModuleOp module);

// Whether an inline assembly string is made only of instructions that
// cannot wait: prefetches, the spin-wait hint, fences, the time-stamp
// counter, no-ops, or nothing at all. With `inert`, fences, the spin-wait
// hint and the empty string (a compiler barrier) are refused as well: what
// is left neither waits nor orders memory.
bool onlyNonBlockingInstructions(llvm::StringRef text, bool inert = false);

// One operation that may block, and how it is reached: the calls from the
// code asked about to it (`chain`), and the function each of them enters
// (`via`). A link of the chain may be a wtc operation rather than a call:
// the operation runs the element's special member it enters. `callee` is the function without a body the leaf calls, if it
// is such a call.
struct BlockingSite {
    Operation *leaf;
    BlockingKind kind;
    std::string why;  // "waits in the threading library (pthread_mutex_lock)"
    cir::FuncOp callee;
    bool threading = false;
    llvm::SmallVector<Operation *, 8> chain;
    llvm::SmallVector<cir::FuncOp, 8> via;
};

// The special members of a queue operation's element that the operation
// runs (ProgressSpec::element), found in the module through ClangIR's
// record of special members.
class ElementMembers {
public:
    explicit ElementMembers(ModuleOp module);
    struct Found {
        unsigned flags = 0;  // what the operation runs, narrowed by the push's form
        bool unknown = false;  // the element type could not be read
        llvm::SmallVector<cir::FuncOp, 4> funcs;  // in the order the operation runs them
    };
    Found of(Operation *op, unsigned flags);

private:
    // For each record type, its special members that are not trivial,
    // with the ElementCode flag each stands for.
    llvm::DenseMap<Type, llvm::SmallVector<std::pair<unsigned, cir::FuncOp>, 4>> members;
    // For each queue's record type, its element type, read from the shim's
    // operations on it.
    llvm::DenseMap<Type, Type> elementOfQueue;
};

class BlockingQuery {
public:
    // A pass's own refusal at an operation: in the code it asked about, or
    // (`inCallee`) in a function that code calls.
    using Extra = std::function<std::optional<BlockingSite>(Operation *op, bool inCallee)>;

    // An operation the asking pass accepts outright (a wait for data that
    // parks, on a real-time thread): no site, though what it contains, for
    // an operation with regions, is still walked.
    using Accept = std::function<bool(Operation *op)>;

    explicit BlockingQuery(ModuleOp module, BlockingKinds accepted = {}, Extra extra = {}, Accept accept = {});

    // Whether op, or anything it calls, may block in a way not accepted.
    std::optional<BlockingSite> at(Operation *op) { return siteAt(op, /*inCallee=*/false); }
    // Every operation of f's own body that may block, each with the first
    // chain found below it, once per distinct chain of source locations and
    // reason.
    llvm::SmallVector<BlockingSite> sitesIn(cir::FuncOp f);

private:
    BlockingKinds accepted;
    Extra extra;
    Accept acceptOp;
    llvm::DenseMap<Operation *, std::optional<BlockingSite>> memo;
    llvm::DenseSet<Operation *> inProgress;
    // Set when a walk met a function still being walked further up: its
    // findings are not in yet, so a "nothing blocks" answer reached under
    // it is only provisional and must not be remembered.
    bool metCycle = false;
    // Stack slots holding the shared_ptr of a borrowed pop of an inline
    // queue: the destructor run on them frees nothing.
    llvm::DenseSet<Value> ownerlessSlots;

    bool ownerless(Value v) const;
    std::optional<BlockingSite> siteAt(Operation *op, bool inCallee);
    std::optional<BlockingSite> callSite(cir::CallOp call);
    std::optional<BlockingSite> firstIn(cir::FuncOp f);
    std::optional<BlockingSite> site(Operation *leaf, BlockingKind kind, std::string why) const;
    // Entering t from `from`: t's own answer when it has no body here or
    // one that may be replaced, else the first site in its body, with
    // `from` and t put in front of the chain.
    std::optional<BlockingSite> enter(Operation *from, cir::FuncOp t, bool nothrow);

    // The element's special members a queue operation runs, followed as
    // calls it makes.
    std::optional<BlockingSite> elementSite(Operation *op, unsigned flags);
    ElementMembers elements;
};

// The element type of a queue operation: the pointee of the value a push
// is handed or of the object a transfer moves into, or, for a pop that
// returns a shared_ptr, the element of the queue's other operations
// (`byQueue`). Null when it cannot be read.
Type queueElementType(Operation *op, const llvm::DenseMap<Type, Type> &byQueue);

// The special member a link of a blocking chain enters, in words, for a
// note: "the element's move-assignment".
std::string describeSpecialMember(cir::FuncOp f);

} // namespace mlir::wtc
