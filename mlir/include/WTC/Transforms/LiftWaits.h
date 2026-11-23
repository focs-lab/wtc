#ifndef WTC_TRANSFORMS_LIFTWAITS_H
#define WTC_TRANSFORMS_LIFTWAITS_H

// Lifting a class's own wait off its condition variable (docs/lifting.md,
// "Lifting a wait"), run by --wtc-lift-locked-queues after the queue
// classes are read.
//
// A thread pool as written keeps its tasks in a queue and its workers wait
// on a condition variable of the pool's, under a mutex of the pool's,
// with a predicate that pops the queue or reads a stop flag:
//
//     std::unique_lock<std::mutex> lock(this->mutex);
//     this->cv.wait(lock, [&] { isPop = this->q.pop(f); return isPop || this->isDone; });
//
// and every push notifies under that mutex. When the mutex and the
// condition variable are used for that wait only, and the predicate reads
// nothing that needs the mutex (a queue whose operations are atomic on
// their own, lifted or under the queue class's own mutex, and atomic
// flags), the wait becomes the runtime's eventcount (runtime/waiter.hpp,
// through shim/lifted.hpp's wtc::lifted_wait<P>): the waiting thread no
// longer takes the mutex, and a notification is the eventcount's, a
// futex wake only when a waiter sleeps. The sections that notify keep
// the mutex; they must write nothing the predicate reads.

#include "WTC/Transforms/ClosedWorld.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"

#include <string>

namespace mlir::wtc {

// A queue class the lifting pass read: its operations are each atomic,
// lifted or under the class's own mutex (it passed conditions (a) to (e)).
struct QueueClassForWaits {
    cir::RecordType rec;
    // Its try-pops, which a predicate may call on it.
    llvm::DenseSet<Operation *> tryPops;
    // Its pushes: the producers of a wait that pops it are their threads.
    llvm::DenseSet<Operation *> pushes;
    // Every member function of the class, operations, constructors and
    // destructors: what writes the queue.
    llvm::DenseSet<Operation *> functions;
};

// Reads every class with a condition variable and a mutex, remarks on its
// wait, and with `rewrite` replaces the wait of every class that lifts.
// Returns the explicit instantiations of wtc::lifted_wait<P> the module
// lacks, one per line, for the classes that would lift.
std::string liftWaits(ModuleOp module, const ClosedWorld &world, llvm::ArrayRef<QueueClassForWaits> queues,
                      bool rewrite);

}  // namespace mlir::wtc

#endif
