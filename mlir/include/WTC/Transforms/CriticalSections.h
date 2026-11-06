#ifndef WTC_TRANSFORMS_CRITICALSECTIONS_H
#define WTC_TRANSFORMS_CRITICALSECTIONS_H

// The critical sections of a module, for the lifting passes and for the
// idioms to come (docs/lifting.md): where a mutex is held, on which object,
// and what is read and written there.
//
// A critical section is taken in one of two forms:
//
//   - a lock object, std::lock_guard, std::unique_lock or std::scoped_lock
//     on one mutex, whose section is the body of the cleanup scope that
//     destroys it, up to a call of the lock's own unlock();
//   - pthread_mutex_lock(m), up to pthread_mutex_unlock(m) on the same
//     mutex in the same block. An unlock in a branch that then returns
//     (an early exit) does not end the section for the code after the
//     branch; one in a branch that falls through does.
//
// The mutex, and what the section reads and writes, are member paths:
// a function argument (`this`, or a pointer parameter), or a global, and
// the members stepped into from it. Locals are the function's own and are
// left out. A direct call made inside a section adds what the callee itself
// reads and writes through its pointer parameters, re-rooted at what the
// call passes: one level into the functions a section calls, so that a
// member function that takes no lock but is called only with its object's
// mutex held (BS::thread_pool's pop_task) is seen as under that mutex.

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mlir::wtc {

// A place in memory reached from a function argument or a global through
// member steps: "arg0.2.1", "@gq.0".
struct MemberPath {
    Value root;          // a block argument of the function's entry, or a cir.get_global's result
    std::string global;  // the global's name when the root is one
    llvm::SmallVector<unsigned, 4> path;

    bool sameRoot(const MemberPath &o) const {
        return global.empty() ? (o.global.empty() && root == o.root) : global == o.global;
    }
    bool operator==(const MemberPath &o) const { return sameRoot(o) && path == o.path; }
    std::string str() const;
};

// The member path an address names, through casts that keep the object,
// the slots parameters arrive in, and member steps; nothing for a local,
// an element of an array, or an address loaded from memory.
std::optional<MemberPath> memberPathOf(Value addr);

struct CriticalSection {
    cir::FuncOp f;
    Operation *lock = nullptr;    // the lock object's constructor call, or pthread_mutex_lock
    Operation *unlock = nullptr;  // the unlock that ends it in its own block, if any
    MemberPath mutex;
    // The operations that run with the mutex held, at every depth.
    llvm::DenseSet<Operation *> inside;
    // Member paths read and written there (MemberPath::str), what the
    // calls one level down read and write included.
    std::set<std::string> reads, writes;
    llvm::SmallVector<cir::CallOp, 4> calls;  // direct calls made inside

    bool contains(Operation *op) const;
};

class CriticalSections {
public:
    explicit CriticalSections(ModuleOp module);

    llvm::ArrayRef<CriticalSection> in(cir::FuncOp f) const;
    // The innermost section an operation runs in, if any.
    const CriticalSection *enclosing(Operation *op) const;
    // The calls of f, when every use of f is a direct call made inside a
    // critical section on member `mutexMember` of the object passed as f's
    // argument `self`; empty when f has no calls or one is made otherwise.
    llvm::SmallVector<cir::CallOp, 2> callsUnderLock(cir::FuncOp f, unsigned self, unsigned mutexMember) const;

private:
    ModuleOp module;
    llvm::DenseMap<Operation *, std::vector<CriticalSection>> sections;
};

}  // namespace mlir::wtc

#endif
