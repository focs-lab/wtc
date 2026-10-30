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
    // The pushes of a bounded queue (shim/bounded.hpp): one that waits while
    // the queue is full, one that answers whether it appended, one that
    // overruns the oldest item.
    QueuePushWait,
    QueueTryPush,
    QueuePushOverrun,
    // A run of pushes from, or pops into, a ring of the calling thread's
    // (shim/c_queue.h): what a C program's batched enqueue and dequeue
    // become.
    QueuePushBatch,
    QueuePopBatch,
    QueueTryPop,
    QueueTryPopInto,
    QueuePopWait,
    QueuePopWaitFor,
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

// Whether a call is part of the implementation of an operation of a
// lifted queue (shim/lifted.hpp): it sits in a function annotated
// wtc_lifted_* and calls a function that is not one. wtc::lifted's own
// functions are the queue's operations; the wtc::queue calls inside them
// implement those and are neither raised nor counted again.
bool insideLiftedRuntime(cir::CallOp op);

bool hasAnnotation(cir::FuncOp funcOp, StringRef annotation);

bool isWTCAnnotatedCall(cir::CallOp op, StringRef annotation);

SemanticOpKind getSemanticKind(cir::FuncOp funcOp);

// Queues of libraries the analysis has a model of, read from the callee's
// name: DPDK's rte_ring and rte_mempool, and the single-producer
// single-consumer queues of rigtorp (SPSCQueue), folly
// (ProducerConsumerQueue), boost (lockfree::spsc_queue) and moodycamel
// (ReaderWriterQueue, BlockingReaderWriterQueue). Their threads are counted
// like a shim queue's and what they declare is checked; nothing about them
// is rewritten. A model is a reading of the library's API, stated as such:
// the method names in Utils.cpp, and DPDK's naming, in which an
// rte_ring_sp_* or rte_ring_sc_* call is the single-thread form of its
// side whatever the ring's flags say, rte_ring_mp_* and rte_ring_mc_* the
// multi-thread form, and the rest follow the flags. A mempool is a queue
// of free objects: a put (rte_mempool_put*, an mbuf's free) pushes, a get
// (rte_mempool_get*, an mbuf's allocation) pops, and its ops -- named, or
// chosen by its flags -- declare the sides.
enum class LibraryQueueRole {
    None,
    Create,     // rte_ring_create, rte_mempool_create: the result is the queue
    Construct,  // a constructor, rte_ring_init, rte_mempool_set_ops_byname: an operand is the queue
    Push,
    Pop,
};
// What decides how many threads a call may share its side with.
enum class LibraryQueueSync {
    ByType,   // a single-producer single-consumer class: one each
    ByFlags,  // rte_ring_enqueue, rte_mempool_put and the like: the queue's flags or ops
    Single,   // rte_ring_sp_*, rte_ring_sc_*: one, whatever the flags
    Multi,    // rte_ring_mp_*, rte_ring_mc_*: any number
};
// Which library a function is of.
enum class LibraryQueueKind { Class, Ring, Mempool };
// How a free reaches the pool it puts into, when the pool is not an
// operand but the `pool` field of the mbuf it is handed.
enum class MbufFree {
    None,      // the queue is the operand
    Own,       // rte_mbuf_raw_free: the mbuf's own pool, one segment
    Chain,     // rte_pktmbuf_free and the like: the pools of every segment
               // of the chain, and of a buffer an indirect mbuf is attached to
    Attached,  // rte_pktmbuf_detach: the pool of the buffer it is attached to
};
// A function as the model reads it. `member` is set for every function of
// a modelled class or of DPDK's ring, mempool and mbuf libraries, whatever
// its role: its body is the library's, not the program's.
struct LibraryQueueFunction {
    LibraryQueueRole role = LibraryQueueRole::None;
    LibraryQueueSync sync = LibraryQueueSync::ByType;
    LibraryQueueKind kind = LibraryQueueKind::Class;
    unsigned queueArg = 0;               // the queue, counted over the source's arguments
    std::optional<unsigned> flagsArg;    // a ring's or a mempool's flags, at its creation
    std::optional<unsigned> opsArg;      // a mempool's ops, by name
    MbufFree viaMbuf = MbufFree::None;   // the queue argument is an mbuf (or an array of them)
    bool returnsMbuf = false;            // hands back an mbuf taken from the queue argument
    // A function of DPDK's mempool or mbuf library that the model does not
    // read and does not know to be inert: handed a pool or an mbuf, it may
    // put or get, so every pool is unknown (QueueOwnershipAnalysis).
    bool unmodelled = false;
    bool member = false;
    explicit operator bool() const { return role != LibraryQueueRole::None; }
};
// Read from the name each time it is asked; a pass that asks often keeps
// its own answers (QueueOwnershipAnalysis does, per run).
LibraryQueueFunction libraryQueueFunction(cir::FuncOp f);
// The same for one call, with the operands counted over all of the call's.
struct LibraryQueueCall {
    LibraryQueueRole role = LibraryQueueRole::None;
    LibraryQueueSync sync = LibraryQueueSync::ByType;
    LibraryQueueKind kind = LibraryQueueKind::Class;
    unsigned queueOperand = 0;           // unused for Create
    std::optional<unsigned> flagsOperand;
    std::optional<unsigned> opsOperand;
    MbufFree viaMbuf = MbufFree::None;
    bool returnsMbuf = false;
    explicit operator bool() const { return role != LibraryQueueRole::None; }
};
LibraryQueueCall libraryQueueCall(cir::CallOp call, const LibraryQueueFunction &callee);
LibraryQueueCall libraryQueueCall(cir::CallOp call);
bool isLibraryQueueFunction(cir::FuncOp f);
// DPDK's mbuf calls that make one mbuf reach another's pool: a chain
// (rte_pktmbuf_chain) or an indirect mbuf (rte_pktmbuf_attach,
// rte_pktmbuf_attach_extbuf, rte_pktmbuf_clone). A free then puts into
// more pools than the one the mbuf was taken from.
bool isMbufLinkFunction(llvm::StringRef name);

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

// Functions without a body here that read and write only the memory they
// are handed, and do nothing else: no input or output, no allocation, no
// waiting. One list for every pass that asks; each adds what its own
// question also allows (the walk: functions that write no object it
// tracks; the lock batching: the allocator, which cannot wait for a mutex
// the program holds).
bool touchesOnlyItsArguments(cir::FuncOp f);

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
// through, at every direct call of the enclosing function when that
// function has internal linkage. Empty when any of that is unknown, when
// the enclosing function can be called from another translation unit, or
// when its own address is taken.
llvm::SmallVector<cir::FuncOp, 4> indirectCallees(cir::CallOp call);

// Every call to a shim queue constructor, keyed by the object it constructs:
// a cir.global, or a cir.alloca, reached from the constructor's `this`
// through traceToRoot. Constructors whose object is anything else are left
// out, so a queue missing from the map has no constructor this can name.
llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>>
collectQueueConstructors(ModuleOp module);

// A loop whose number of iterations the program can compute before it
// runs: `for (i = first; i < bound; ++i)`, or `<=`, where i is a slot that
// only the loop's step writes, by one, and the bound is loaded from a
// global or a stack slot in the condition each time round. Whether the
// bound can change while the loop runs is not judged here; the caller that
// computes the count elsewhere has to establish that.
struct CountedLoop {
    cir::ForOp loop;
    cir::LoadOp bound;      // the condition's load of the bound
    int64_t first = 0;      // what i is set to just before the loop
    bool inclusive = false; // i <= bound
};
std::optional<CountedLoop> countedLoop(cir::ForOp loop);

// Whether the queue whose root this is was found spsc, or would be once each
// counted loop recorded on it (kProducersLoopAttrName, kConsumersLoopAttrName)
// runs at most once: every side with many threads has its loop recorded.
bool spscWhenLoopsRunOnce(Operation *root);

// Whether a call of the shim's queue constructor passes a topology tag, as
// its second operand: queue(topology), or queue(topology, wait_policy),
// whose policy the analyses leave alone, or wtc::lifted_bounded's
// construct(at, topology, capacity, overrun).
bool takesTopologyTag(cir::CallOp ctor);

// The topology tag a queue's one constructor passes, if it is a constant;
// nullopt when there is not exactly one constructor that takes a tag
// (takesTopologyTag), or its tag is not a constant.
std::optional<int64_t> constructorTag(llvm::ArrayRef<cir::CallOp> ctors);

}
