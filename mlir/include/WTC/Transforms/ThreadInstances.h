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
// spawn entry, which may be called from any thread any number of times; any
// remaining externally visible function nobody in this translation unit
// calls; and, when the client says another translation unit may call it
// (setCallableElsewhere), an externally visible function this one calls as
// well. Each of the last three stands for any number of threads the module
// cannot account for. Which other units exist is the client's to say: the
// walk sees one translation unit, and docs/audit.md says what the queue
// analysis assumes of the rest of the program and what it checks.

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Interfaces/DataLayoutInterfaces.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <deque>
#include <functional>
#include <optional>
#include <utility>

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
    // True when the instance's Many is a guess: some call or spawn on its
    // chain sits in a function whose control flow is unstructured, outside
    // any loop, so the site could run once or many times, and nothing else
    // on the chain is known to repeat. The multiplicities above keep the
    // guess, so a client that must assume the worst reads Many; a client
    // that would turn Many into a claim treats such an instance as neither
    // once nor repeated.
    bool execUnknown = false;
    // True for an instance whose function already appears on its chain.
    // The first such instance of each function is expanded once more, so
    // that what the recursive call reaches is seen; later ones are not,
    // which bounds the walk on a group of mutually recursive functions.
    bool recurring = false;
    // True when whether this instance runs before the first spawn cannot be
    // read: a caller on the way has unstructured control flow and can
    // spawn, so its spawn may come before or after the call. Reached
    // without passing a spawn, as far as the walk can tell, and that is a
    // guess.
    bool startupUnknown = false;
    // True for a recurring instance that was visited but not expanded: what
    // its calls reach is not in the walk. A client that would call a pair
    // cold must not, on this thread, since the unseen calls may touch it.
    bool truncated = false;
    // For a root standing for any number of threads, why it may run any
    // number of times, as a remark can say it after a colon: its address
    // is taken, or nothing in this module calls it. Null for main and the
    // global constructors, and for every instance with a parent, whose
    // multiplicity comes from its chain.
    const char *manyRoot = nullptr;
    // For a thread started by a spawn that runs once in each iteration of a
    // counted loop (countedLoop in Utils.h), with nothing else repeating
    // around it, in an instance that runs once: that loop. The thread then
    // stands for exactly as many threads as the loop has iterations, a
    // number the program knows before the loop runs. threadMult says Many
    // all the same, which is what holds whatever the number; every
    // instance on the thread carries the loop.
    Operation *countLoop = nullptr;
    // With countLoop: the instance of the function that holds the loop,
    // the one run of it whose loop started this thread. A client that
    // credits a thread to the loop of the object it touches compares this
    // with the instance that built the object.
    const Instance *countLoopInst = nullptr;
};

// Where a handle points, as far as the walk can read it: a root (a
// cir.global, a cir.alloca, or the allocator call that made the object)
// and how the handle got there.
// Why a handle could not be resolved to a root, for the summary's account
// of what the analysis does not reach.
enum class Unplaced {
    None,
    LoadedField,     // a pointer loaded from a field of another object
    LoadedGlobal,    // a pointer loaded from a global written more than once, or named elsewhere
    LoadedSlot,      // a pointer loaded from a stack slot written more than once
    RootParameter,   // a parameter of a root instance: nobody passed it
    CallResult,      // the result of a call that is not an allocation
    Integer,         // cast from an integer
    ManySources,     // a loaded pointer with more sources, or sources in more instances, than the walk will carry
    Other
};

struct Place {
    Operation *root = nullptr;
    Unplaced cause = Unplaced::None;   // set when root is null
    // Bytes from the root to where the handle points, summed over element
    // steps with constant indices on the way: `&arr[2]` is 2 elements in.
    uint64_t offset = 0;
    // Set when the handle came through `&arr[i]` at a spawn inside a loop
    // whose induction variable is i: the thread's accesses are within one
    // element of the array, `elem`, at offsets relative to it, and which
    // element it is depends on the thread.
    Operation *loop = nullptr;
    Type elem;
    // With `loop`: the loop steps by one, spawns unconditionally, is the
    // only repetition around the spawn, and the spawning instance runs
    // once, so every element in the loop's range has exactly one thread
    // and the threads of neighbouring elements both exist.
    bool owner = false;
    // Set when an element step on the way had an index the walk cannot
    // read: the handle points into some element of the root, at an offset
    // within it, and which element is not known.
    bool unknownIndex = false;
    // Set when the handle was loaded from a slot, a global or a field
    // written in several places, each of which the walk could place: this
    // is one of the places it may name, and the others are reported
    // alongside. A statement about a set, so never a definite verdict.
    bool oneOf = false;
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

    // Functions another translation unit may call, whether or not this one
    // does: each is a root standing for any number of threads the module
    // cannot account for, beside its callers here. Set before run(); by
    // default only the functions nothing here calls are (addOrphanRoots).
    void setCallableElsewhere(std::function<bool(cir::FuncOp)> calledElsewhere) {
        callableElsewhere_ = std::move(calledElsewhere);
    }

    // The root object a handle refers to -- a cir.global, a cir.alloca, or
    // the cir.call that allocated it -- resolved through the chain of call
    // sites that created this instance, through a stack slot written once,
    // and through a pointer-typed global written once (memcached's
    // `threads = calloc(...)`). Null when the trail does not end at one,
    // which the caller must treat as unresolved rather than as absent.
    Operation *resolveRoot(Value v, Instance *inst, unsigned depth = 0) const;

    // resolveRoot through element steps as well: a constant index adds to
    // the offset, the spawn loop's induction variable makes the place
    // per-thread, any other index makes the element unknown (see Place).
    // A null root means unresolved, as for resolveRoot.
    Place resolvePlace(Value v, Instance *inst) const;

    // resolvePlace, following a pointer loaded from a slot, a global or a
    // field written in several places: every value stored there is
    // placed where its store runs, and the handle is one of those places
    // (Place::oneOf), unless any of them cannot be placed, in which case
    // nothing is. One place with a null root means unresolved, with the
    // cause set; several places all carry oneOf; one place without it is
    // as certain as resolvePlace's.
    llvm::SmallVector<Place, 4> resolvePlaces(Value v, Instance *inst) const;
    // Every instance of a function the walk made, none for a function it
    // did not enter.
    llvm::ArrayRef<Instance *> instancesOf(cir::FuncOp f) const;
    // More sources than this, or more instances of a storing function
    // than this, and the load stays unplaced: a set that large says
    // nothing about where the pointer points.
    static constexpr unsigned kMaxSources = 8;
    static constexpr unsigned kMaxStoreInstances = 64;

    // How many times the instance's function runs within its own thread:
    // the sites on the chain from the thread's entry down to it, joined,
    // with a recurrence counting as many. A thread spawned in a loop stands
    // for many threads, and for a place each of those threads owns alone
    // that multiplicity is spread over the elements, so what repeats on one
    // element is only what repeats inside the thread. `known` is false
    // when a site on the chain is a guess (a goto).
    Mult multWithinThread(const Instance &inst, bool &known);

    // Whether an operation runs at most once in the program, read from the
    // call graph: not in a loop, in a function that runs once -- main, or a
    // function called from exactly one site that itself runs once. The
    // instance-based runsOnce answers only for functions the walk entered;
    // an allocation in an init() that touches no field has no instance.
    bool siteRunsOnce(Operation *op, unsigned depth = 0);

    // The value stored into a pointer-typed global, when the module stores
    // into it exactly once and only ever loads it otherwise; null when it
    // is written twice (realloc, re-initialisation, `p = NULL` at shutdown)
    // or its address goes anywhere else. Computed once per module.
    Value singleStoredGlobalValue(cir::GlobalOp g) const;

    // The function a spawn starts, resolved the same way: through casts and
    // single-assignment slots to a named function, or, when the entry is a
    // parameter, through the call site that passed it. memcached starts its
    // workers with create_worker(worker_libevent, ...), whose pthread_create
    // names only its own parameter.
    cir::FuncOp resolveEntry(Value v, Instance *inst, unsigned depth = 0) const;

    // Every function a spawn's entry operand may be: resolveEntry's one,
    // or, for an entry loaded from a field that holds only spawn entries
    // (computeEntryFields), the functions stored into that field of the
    // object the load reads -- ferret's tpool_create starts each
    // descriptor's start_routine, and main fills one array of descriptors
    // per stage -- or, when that object cannot be told from others of its
    // record, every function stored into the field anywhere. Several are
    // several possible threads, each counted. Empty when none is known.
    void resolveEntries(Value v, Instance *inst, llvm::SmallVectorImpl<cir::FuncOp> &out) const;

    // The function a std::thread starts. libstdc++'s constructor is a
    // template instantiated per callable: it allocates a
    // _State_impl<_Invoker<tuple<F, Args...>>>, moves the callable into it
    // and hands it to the bodiless _M_start_thread, and the new thread runs
    // that state's _M_run, whose only mention in the module is the state's
    // vtable. The state's type, read off the constructor call that builds
    // it in the same function as `start`, names the entry. Null when
    // `start` is not that call or the module holds no such _M_run.
    cir::FuncOp stdThreadEntry(cir::CallOp start);

    // The callable a std::thread runs, from the constructor instance that
    // built it: the second operand of the constructor call, through
    // single-assignment slots, copies out of a member-pointer constant and
    // the call chain of `ctx`, to a named function. Null when that trail
    // does not end at one.
    cir::FuncOp resolveCallable(Value v, Instance *ctx) const;

    // Whether f, or anything it reaches by a direct call or by spawning a
    // thread, contains an operation the predicate accepts. Computed for the
    // whole module on the first call, for the predicate passed then: pass the
    // same predicate each time.
    bool reachesOp(cir::FuncOp f, llvm::function_ref<bool(Operation *)> pred);

    // Many if the operation sits inside a loop of its function, or the
    // function's control flow is not structured enough to tell. The
    // three-argument form also says which: `known` is false only when the
    // answer is a guess, that is, the function has a goto and the operation
    // is not inside a loop. A client that must assume the worst keeps the
    // guess; a client that would turn it into a claim treats it as unknown.
    Mult siteMult(Operation *op, cir::FuncOp f);
    Mult siteMult(Operation *op, cir::FuncOp f, bool &known);

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

    // Whether the answer above is a guess: the instance was reached
    // through, or sits in, a function with unstructured control flow that
    // can spawn, so where its start-up phase ends cannot be read. A guess
    // makes no definite verdict and no cold one.
    bool startupUncertain(const Instance &inst) {
        return inst.startupUnknown ||
               (inst.startup && isUnstructured(inst.func) && canSpawn(inst.func));
    }

    // Whether the function's control flow is structured enough for siteMult
    // to mean what it says. With a goto in the function, siteMult answers
    // Many so that clients which must assume the worst do; a client that
    // would turn "many" into a claim asks here first, and treats a function
    // that is not known as neither once nor many.
    bool rateKnown(cir::FuncOp f) { return !isUnstructured(f); }

    // Whether two accounted-for threads cannot both exist: each was started
    // from a different arm of one `if` (or ternary) in the function that
    // started them both, directly or through calls, and neither that
    // function nor any caller above it has control flow its regions do not
    // show (a goto, a setjmp). `if (c) spawn(w) else
    // spawn(w)` is one thread, and the walk, which numbers every spawn
    // instance, used to hand it two numbers and then report the fields it
    // touches as shared between them.
    bool exclusive(unsigned threadA, unsigned threadB) const;

    // Whether each of two accounted-for threads is started under a
    // condition the other's start is not under -- `if (!f) spawn(a); ...
    // if (f) spawn(b);` -- so that the two conditions may never hold
    // together and nothing shows that both threads exist. The conditions
    // are any if, ternary or switch case between the function that started
    // them both, below the point where their paths part, and each spawn,
    // in that function and in the callees on the way. Two ifs on one local
    // with the same sense and no write between are one condition, not two.
    // Neither exclusive nor
    // the opposite of it: a claim that two threads both exist asks here.
    bool bothConditional(unsigned threadA, unsigned threadB) const;

    // How many instances the walk created for f. A stack slot of f is one
    // object only if f ran once.
    unsigned instanceCount(cir::FuncOp f) const;
    // Whether f has exactly one instance and that instance runs exactly
    // once on exactly one thread. A slot of such a function is one object.
    bool runsOnce(cir::FuncOp f) const;

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
    bool isUnstructured(cir::FuncOp f) const;
    bool hasIndirectCall(cir::FuncOp f);
    bool isRecursive(cir::FuncOp f);
    bool canSpawn(cir::FuncOp f);
    bool indirectCallCanSpawn();
    const llvm::DenseSet<Operation *> &preSpawnOps(cir::FuncOp f);
    Instance &addInstance(Instance inst);
    Instance *addIfRoom(Instance inst);
    // `many` receives the places of a pointer loaded from somewhere written
    // more than once, when given; the call then returns null with the
    // cause unset. `inst` may be null: the value is then resolved without
    // a call string, so a parameter is a root.
    Operation *resolveRootImpl(Value v, Instance *inst, unsigned depth, cir::CallOp via,
                               Place *place, llvm::SmallVectorImpl<Place> *many) const;
    // The places a pointer loaded from `at` (a slot, a pointer global or a
    // field) may be, each value stored there resolved where its store
    // runs; false when any of them is not placed, or there are too many,
    // with `why` moved from the load's own reason to that source's.
    bool placesOfLoad(Value at, Instance *inst, unsigned depth,
                      llvm::SmallVectorImpl<Place> &out, Unplaced &why) const;
    bool resolveSources(llvm::ArrayRef<cir::StoreOp> stores, Instance *ctx, bool perInstance,
                        unsigned depth, llvm::SmallVectorImpl<Place> &out, Unplaced &why) const;
    // The place a constant pointer names: a global, whole. False for
    // anything else, a view into part of a global included.
    bool placeConstant(Attribute a, llvm::SmallVectorImpl<Place> &out) const;
    void computeFieldStores() const;
    // The fields that hold a thread's entry and nothing else: a field of
    // function pointer type in a record that the module keeps to itself --
    // never held by value in another record or a global, never handed to
    // a function with no body but free, its pointers made only from an
    // allocation and turned into others only for free -- whose every store
    // stores a named function or null, and whose every load is compared
    // or handed to a spawn as its entry. A function stored there is no
    // address taken: it runs only as the thread such a spawn starts, which
    // resolveEntries reads back. Computed once per module.
    void computeEntryFields() const;
    // The entry field an entry operand is loaded from, and every function
    // stored into that field anywhere, appended to `out`; null when the
    // operand is no load of an entry field.
    cir::GetMemberOp entryFieldFunctions(Value v, llvm::SmallVectorImpl<cir::FuncOp> &out) const;
    const llvm::DenseSet<std::pair<Type, unsigned>> &entryFieldSet() const {
        if (!entryFields_) computeEntryFields();
        return entryFieldKeys_;
    }
    struct EntryField {
        llvm::SmallVector<cir::StoreOp, 4> stores;   // of a function or null
    };
    mutable std::optional<llvm::DenseMap<std::pair<Type, unsigned>, EntryField>> entryFields_;
    mutable llvm::DenseSet<std::pair<Type, unsigned>> entryFieldKeys_;
    // For each record with an entry field: its copies (cir.copy) and the
    // stores of a whole value of it, which carry the field from one object
    // to another.
    mutable llvm::DenseMap<Type, llvm::SmallVector<cir::CopyOp, 4>> entryRecordCopies_;
    mutable llvm::DenseMap<Type, llvm::SmallVector<cir::StoreOp, 4>> entryRecordStores_;
    // Whether `index` is the induction variable of `loop`: a slot whose
    // every store is either outside the loop or in its step region.
    // `unitStep` is set when the step region advances it by exactly one.
    static bool isInductionOf(Value index, Operation *loop, bool &unitStep);
    // The counted loop whose iterations each start one thread at `spawn`
    // in `inst`, when that loop is the whole of the spawn's repetition. The
    // loop may be in `inst`'s own function or in a caller above it, with
    // the spawn reached by calls that each run once per iteration and
    // each start exactly one thread per call. `holder` receives the
    // instance whose function holds the loop.
    Operation *countingLoop(Operation *spawn, const Instance &inst, const Instance **holder);
    // Whether every run of the region, or every call of the function,
    // starts exactly one thread: one spawning call, or one spawning
    // callee that does, on every path, with no loop, jump or early return
    // around it.
    bool spawnsOnce(Region &r, unsigned depth);
    bool spawnsOncePerCall(cir::FuncOp f, unsigned depth);
    bool canSpawnIn(Operation *op);
    bool canSpawnIn(Region &r);
    // The functions with indirect calls that a std::thread's _M_run
    // reaches, and the named functions those calls may run: the callables
    // handed to the thread constructor at every site that builds that
    // state. `unresolved` when some site's callable could not be read.
    struct InvokeTargets {
        llvm::SmallVector<Operation *, 4> funcs;
        bool unresolved = false;
    };
    void computeInvokeTargets();
    const DataLayout &dataLayout() const;
    void noteRemark(Operation *at, const llvm::Twine &msg);
    void addRootsForModule();
    void expand(Instance &inst);
    void addOrphanRoots();
    void computeSpawns();

    ModuleOp module_;
    llvm::function_ref<bool(cir::FuncOp)> relevant_;
    std::function<bool(cir::FuncOp)> callableElsewhere_;
    unsigned cap_;
    bool reachesComputed_ = false;
    unsigned nextThread_ = 0;
    std::deque<Instance> instances_;   // stable addresses: Instance::parent points into it
    llvm::DenseSet<Operation *> reachedFuncs_;
    mutable llvm::DenseMap<Operation *, bool> unstructuredCache_;
    llvm::DenseMap<Operation *, bool> recursiveCache_;
    bool recursiveComputed_ = false;
    llvm::DenseMap<unsigned, const Instance *> threadRoot_;   // the instance that began each thread
    llvm::DenseSet<std::pair<Operation *, uint64_t>> expandedRecurrence_;   // (function, thread/phase) unrolled once
    mutable llvm::DenseMap<Operation *, Value> globalStores_;  // pointer global -> its one stored value
    mutable bool globalStoresComputed_ = false;
    // Pointer global -> every store into it, when nothing else names it.
    mutable llvm::DenseMap<Operation *, llvm::SmallVector<cir::StoreOp, 2>> globalMultiStores_;
    // (record type, field index) -> every store into that field anywhere in
    // the module, or poisoned: the field's address goes somewhere a store
    // would not be seen, or the record is written as a whole.
    struct FieldStores {
        llvm::SmallVector<cir::StoreOp, 4> stores;
        // Values the field takes from a constant: a global's initialiser,
        // or a copy out of a constant global (`pair p{&a, &b}`).
        llvm::SmallVector<Attribute, 2> constants;
        bool poisoned = false;
    };
    using FieldKey = std::pair<Type, unsigned>;
    mutable llvm::DenseMap<FieldKey, FieldStores> fieldStores_;
    mutable bool fieldStoresComputed_ = false;
    // What a load from a field or a global resolves to, kept per field and
    // per global: the answer does not depend on who asks. An entry in
    // progress is a cycle (`s->next = t`), which resolves to nothing.
    struct Candidates {
        llvm::SmallVector<Place, 4> places;
        bool ok = false;
        bool inProgress = false;
        Unplaced why = Unplaced::None;
    };
    mutable llvm::DenseMap<FieldKey, Candidates> fieldCandidates_;
    mutable llvm::DenseMap<Operation *, Candidates> globalCandidates_;
    // Per slot and instance: a chain of locals each assigned on two arms
    // from the previous one was resolved once per path, 2^n times.
    mutable llvm::DenseMap<std::pair<Operation *, const Instance *>, Candidates> slotCandidates_;
    mutable llvm::DenseMap<Operation *, llvm::SmallVector<Instance *, 2>> instancesByFunc_;
    mutable size_t instancesIndexed_ = 0;   // instances_.size() the index and the candidates were built for
    mutable std::optional<DataLayout> dataLayout_;
    llvm::DenseMap<Operation *, llvm::SmallVector<Operation *, 2>> callers_;  // function -> direct call sites
    bool callersComputed_ = false;
    llvm::DenseMap<Operation *, bool> reachesCache_;
    llvm::DenseMap<Operation *, llvm::DenseSet<Operation *>> preSpawnCache_;
    llvm::DenseMap<Operation *, bool> spawnsCache_;
    std::optional<bool> indirectSpawns_;
    // The functions whose address is taken, from one pass over every
    // symbol use in the module: asked per function, the question walked
    // the whole module each time, and on a 1,200-function module with the
    // standard headers that was nine seconds of eleven.
    std::optional<llvm::DenseSet<Operation *>> addressTaken_;
    bool isAddressTaken(cir::FuncOp f);
    // std::thread state record type -> its _M_run, over the module once.
    llvm::DenseMap<Type, Operation *> stateRun_;
    bool stateRunComputed_ = false;
    // The _M_run entries some std::thread constructor resolved to. Only
    // these are exempt from being roots: an _M_run no constructor names
    // -- the library laid the state out some other way -- is still a
    // function whose address is taken, callable from anywhere, rather
    // than a thread silently dropped.
    llvm::DenseSet<Operation *> resolvedRuns_;
    llvm::DenseMap<Operation *, InvokeTargets> invokeTargets_;
    bool invokeTargetsComputed_ = false;
    llvm::DenseMap<Operation *, bool> spawnsOnceCache_;
    bool capExceeded_ = false;
    unsigned remarks_ = 0;
    llvm::SmallVector<std::pair<Operation *, std::string>, 8> pendingRemarks_;
    llvm::DenseSet<Operation *> remarkedAt_;
};

} // namespace mlir::wtc
