#pragma once

// Which thread runs this operation, and how many threads does it stand for.
//
// Every analysis in this project needs the same thing before it can say
// anything: for a given operation, which dynamic threads execute it. The
// queue analysis needed it to count producers and consumers; the layout
// analysis needs it to decide whether two fields are written by different
// threads. The walk is the subtle part of both, so it lives here once.
//
// Threads are dynamic, not static. A spawn site inside a loop, or in a
// function that runs more than once, stands for many threads. A function
// instance is a function under a particular chain of call sites from a
// root, and carries the call site that created it, so a parameter can be
// resolved to the argument that caller passed. That is what makes "which
// object does this access touch" answerable for a function that takes the
// object as a parameter, and what distinguishes two objects passed to the
// same function.
//
// Roots of the walk: main; the functions global constructors run (on the
// main thread, once); every function whose address is taken other than as a
// spawn entry, which may be called from any thread any number of times; and
// any remaining externally visible function nobody in this translation unit
// calls. The whole-program assumption is one translation unit, stated here
// because nothing enforces it.

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <deque>
#include <optional>

namespace mlir::wtc {

enum class Mult { One, Many };

inline Mult join(Mult a, Mult b) {
    return (a == Mult::One && b == Mult::One) ? Mult::One : Mult::Many;
}

struct Instance {
    cir::FuncOp func;
    unsigned thread;
    Mult threadMult;   // how many dynamic threads this instance's thread stands for
    Mult execMult;     // how many times the function runs within that thread
    cir::CallOp site;  // the call or spawn that created this instance; null for a root
    Instance *parent;
    // True when this instance's thread is not one the module can account
    // for: a placeholder for a caller in another translation unit, or a
    // function whose address is merely taken. Both were once distinguished
    // -- "its address is taken here, so it plausibly runs as a thread" --
    // and plausibly is not what a definite verdict claims: two callbacks
    // registered with one single-threaded event loop have their addresses
    // taken too, and were being handed two distinct accounted-for thread
    // identifiers on that basis.
    //
    // A thread is accounted for when it descends from main, from a global
    // constructor, or from a spawn this module can see. Everything else
    // may contribute `possible`, never `definite`.
    bool speculative = false;
    // True while this instance is guaranteed to run before any thread has
    // been spawned. Almost every program writes a great deal of shared
    // state during start-up and then never again, and that state cannot
    // contend with threads that do not exist yet. Without this, memcached's
    // option parsing -- `settings.maxbytes` and its neighbours, written in
    // the getopt loop -- pairs against every worker that later reads the
    // same struct, and the analysis reports 176 findings that are all one
    // start-up phase.
    bool startup = false;
};

class ThreadInstanceWalk {
public:
    // `relevant` decides whether a function deserves an instance. A function
    // that cannot reach any operation the client cares about cannot affect
    // any verdict, so skipping it is not an approximation -- and without it
    // the standard library's own call graph fills the worklist, which is
    // what made the queue analysis give up on any program including <thread>.
    ThreadInstanceWalk(ModuleOp module, llvm::function_ref<bool(cir::FuncOp)> relevant,
                       unsigned cap = kInstanceCap);

    // Calls `visit` once per instance, in deterministic module order.
    // `onRecursion` is called instead of visiting when an instance would
    // repeat a function already on its own call string: the parameter-to-root
    // map would be cyclic, so the client must give up on that subtree rather
    // than guess.
    void run(llvm::function_ref<void(Instance &)> visit,
             llvm::function_ref<void(cir::FuncOp)> onRecursion);

    // The root object a handle refers to -- a cir.global or a cir.alloca --
    // resolved through the chain of call sites that created this instance.
    // Null when the trail does not end at one, which the caller must treat
    // as unresolved rather than as absent.
    Operation *resolveRoot(Value v, Instance *inst, unsigned depth = 0) const;

    // The function a spawn starts, resolved the same way: through casts and
    // single-assignment slots to a named function, or, when the entry is a
    // parameter, through the call site that passed it. memcached starts its
    // workers with create_worker(worker_libevent, ...), whose pthread_create
    // names only its own parameter.
    cir::FuncOp resolveEntry(Value v, Instance *inst, unsigned depth = 0) const;

    // Whether f, or anything it reaches by a direct call or by spawning a
    // thread, contains an operation the predicate accepts. Computed for the
    // whole module on the first call, for the predicate passed then: pass the
    // same predicate each time.
    bool reachesOp(cir::FuncOp f, llvm::function_ref<bool(Operation *)> pred);

    // Many if the operation sits inside a loop of its function, or the
    // function's control flow is not structured enough to tell.
    Mult siteMult(Operation *op, cir::FuncOp f);

    // Whether some call that can spawn a thread comes before this operation
    // in its function. An operation inside a loop that contains a spawn
    // counts as preceded, because the second lap follows the first spawn.
    bool spawnPrecedes(Operation *op, cir::FuncOp f);

    // Whether THIS OPERATION runs before any thread has been spawned.
    //
    // Ask this, not `inst.startup`. The flag on the instance says only that
    // the instance was *reached* without passing a spawn; the spawn may well
    // be in its own body, and then everything after it is not start-up. An
    // earlier version read the flag alone, which silenced main entirely in
    // every program that spawns from main.
    //
    // The phase ends only at a spawn the module can see. For std::thread
    // that is the call to the bodiless _M_start_thread, recognised by name;
    // a thread started by a library this module cannot see is not seen to
    // end it, and accesses after it are still taken for start-up.
    bool inStartupPhase(Operation *op, const Instance &inst) {
        return inst.startup && !spawnPrecedes(op, inst.func);
    }

    // Whether the function's control flow is structured enough for siteMult
    // to mean what it says. With a goto in the function, siteMult answers
    // Many so that clients which must assume the worst do; a client that
    // would turn "many" into a claim asks here first, and treats a function
    // that is not known as neither once nor many.
    bool rateKnown(cir::FuncOp f) { return !isUnstructured(f); }

    // Whether two accounted-for threads cannot both exist: each was started
    // from a different arm of one `if` (or ternary) in the function that
    // started them both, directly or through calls. `if (c) spawn(w) else
    // spawn(w)` is one thread, and the walk, which numbers every spawn
    // instance, used to hand it two numbers and then report the fields it
    // touches as shared between them.
    bool exclusive(unsigned threadA, unsigned threadB) const;

    // How many instances the walk created for f. A stack slot of f is one
    // object only if f ran once.
    unsigned instanceCount(cir::FuncOp f) const;

    bool capExceeded() const { return capExceeded_; }
    unsigned cap() const { return cap_; }
    unsigned remarks() const { return remarks_; }
    size_t instances() const { return instances_.size(); }
    // An instance is a function under one call string, so the count grows
    // with the number of paths, not functions. Amalgamated memcached needs
    // 132,997 and about 3.6 GB; the earlier cap of 10,000 was exceeded
    // there without anyone checking, so the verdicts were computed from a
    // walk that had silently stopped. Past the cap every pass refuses.
    static constexpr unsigned kInstanceCap = 250000;

private:
    bool isUnstructured(cir::FuncOp f);
    bool isRecursive(cir::FuncOp f);
    bool canSpawn(cir::FuncOp f);
    bool indirectCallCanSpawn();
    const llvm::DenseSet<Operation *> &preSpawnOps(cir::FuncOp f);
    Instance &addInstance(Instance inst);
    bool addIfRoom(Instance inst);
    void noteRemark(Operation *at, const llvm::Twine &msg);
    void addRootsForModule();
    void expand(Instance &inst);
    void addOrphanRoots();
    void computeSpawns();

    ModuleOp module_;
    llvm::function_ref<bool(cir::FuncOp)> relevant_;
    unsigned cap_;
    bool reachesComputed_ = false;
    unsigned nextThread_ = 0;
    std::deque<Instance> instances_;   // stable addresses: Instance::parent points into it
    llvm::DenseSet<Operation *> reachedFuncs_;
    llvm::DenseMap<Operation *, bool> unstructuredCache_;
    llvm::DenseMap<Operation *, bool> recursiveCache_;
    bool recursiveComputed_ = false;
    llvm::DenseMap<unsigned, const Instance *> threadRoot_;   // the instance that began each thread
    llvm::DenseMap<Operation *, bool> reachesCache_;
    llvm::DenseMap<Operation *, llvm::DenseSet<Operation *>> preSpawnCache_;
    llvm::DenseMap<Operation *, bool> spawnsCache_;
    std::optional<bool> indirectSpawns_;
    bool capExceeded_ = false;
    unsigned remarks_ = 0;
    llvm::SmallVector<std::pair<Operation *, std::string>, 8> pendingRemarks_;
    llvm::DenseSet<Operation *> remarkedAt_;
};

} // namespace mlir::wtc
