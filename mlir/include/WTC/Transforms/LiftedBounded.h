#ifndef WTC_TRANSFORMS_LIFTEDBOUNDED_H
#define WTC_TRANSFORMS_LIFTEDBOUNDED_H

// Two readings the lifting of locked queues needs for queues the program
// bounds itself (LiftLockedQueues.cpp, docs/lifting.md "The bound"):
//
//   - a ring: a record whose member functions, read symbolically, are
//     those of a bounded FIFO ring over a std::vector, with a slot count,
//     a head, a tail and a count of overruns (spdlog's circular_q);
//   - the values an integer may take, through parameters, stack slots,
//     members and forwarding references, for proving that a call the
//     program guards by such a value (the overflow policy that chooses an
//     overrunning push) never runs.

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace mlir::wtc {

// What a member function of a recognised ring does, by its body, whatever
// its name.
enum class RingOp {
    None,
    Push,                 // append at the tail; when full, the oldest is overrun and counted
    Front,                // a reference to the oldest element
    PopFront,             // drop the oldest element (moved out before, through front)
    Empty,                // whether it holds nothing
    Full,                 // whether it holds as many as its capacity
    Size,                 // how many it holds
    OverrunCounter,       // how many elements pushes have overrun
    ResetOverrunCounter,  // set that count to zero
};

const char *ringOpName(RingOp op);

// A record read as a bounded ring. Built from the record's constructor that
// takes the capacity: it must set the slot count to that argument plus one,
// head, tail and the count to zero, and size the vector by the slot count,
// which makes the capacity the argument. Each member function is then
// classified on demand, by comparing what it computes, symbolically, with
// the ring's operations; the assignment of members to head, tail and
// count is the one under which every function asked about so far is one of
// them.
class RingModel {
public:
    RingModel(ModuleOp module, cir::RecordType rec);

    // Why the record is not a ring, or empty.
    const std::string &why() const { return why_; }
    cir::RecordType record() const { return rec_; }
    // The constructor that takes the capacity, as its argument 1.
    cir::FuncOp sizedConstructor() const { return ctor_; }

    // What f does, or None with the reason in `reason`. A function is
    // checked against every assignment of roles still open, and the
    // assignments it rules out stay ruled out.
    RingOp classify(cir::FuncOp f, std::string &reason);

private:
    struct Effect;
    std::optional<Effect> evaluate(cir::FuncOp f, std::string &reason);

    ModuleOp module_;
    cir::RecordType rec_;
    cir::FuncOp ctor_;
    std::string why_;
    unsigned slots_ = 0, vec_ = 0;
    // Head, tail, count: the assignments of the three zeroed members not
    // yet ruled out.
    llvm::SmallVector<std::array<unsigned, 3>, 6> roles_;
    llvm::DenseMap<Operation *, RingOp> done_;
};

// The integer constants a value may hold, read through the whole module:
// a constant; a by-value parameter, from every direct call of its function;
// a load from a stack slot whose address goes only to its own loads and
// stores and to read-only reference parameters; a load through a
// reference parameter, from what every call passes; a load of a member,
// from every store to that member in the module. None when anything on
// the way is unknown: a function whose address is taken or that another
// file may call, a member whose address escapes, a member of a record
// whose storage is reached other than member by member (written whole
// from a constant, copied into as bytes, reached through a pointer of
// another type or a pointer to member, handed to a function with no body
// here, named by another file), a module without main.
class ValueSets {
public:
    using Set = std::set<int64_t>;
    // What the other files of the program may do: call a function of this
    // module, name a record. Both false when the module is the program.
    struct Elsewhere {
        std::function<bool(cir::FuncOp)> calls;
        std::function<bool(cir::RecordType)> names;
    };
    explicit ValueSets(ModuleOp module, Elsewhere elsewhere = {});

    std::optional<Set> of(Value v);

    // Whether the conditions around `op`, up to its function, are false
    // for every value their operands may hold: then `op` never runs. The
    // reason names the condition.
    bool neverRuns(Operation *op, std::string &why);

private:
    std::optional<Set> of(Value v, unsigned depth);
    std::optional<Set> pointee(Value ptr, unsigned depth);
    std::optional<Set> param(cir::FuncOp f, unsigned i, unsigned depth);
    std::optional<Set> paramPointee(cir::FuncOp f, unsigned i, unsigned depth);
    std::optional<Set> member(cir::RecordType rec, unsigned i, unsigned depth);
    std::optional<Set> slot(cir::AllocaOp a, unsigned depth);
    bool readOnlyParam(cir::FuncOp f, unsigned i, unsigned depth);
    // Why the storage of `rec`'s objects may be written other than member
    // by member, or empty when it may not.
    const std::string &exposed(cir::RecordType rec);
    bool contains(Type t, cir::RecordType rec, unsigned depth = 0);
    bool readOnlyUses(Value ptr, unsigned depth);
    std::optional<bool> truth(Value cond, unsigned depth, bool &decided);
    // Records the first reason a value is unknown, and says so.
    std::nullopt_t unknown(Operation *at, const llvm::Twine &what);
    std::string why_;

    ModuleOp module_;
    bool closed_ = false;  // the module has main: every caller is in it
    llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 4>> callers_;
    llvm::DenseSet<Operation *> escaping_;  // functions used other than by direct calls
    Elsewhere elsewhere_;
    std::map<const void *, std::string> exposed_;
    std::set<std::pair<Operation *, unsigned>> inProgress_;
    std::set<std::pair<const void *, unsigned>> membersInProgress_;
};

} // namespace mlir::wtc

#endif
