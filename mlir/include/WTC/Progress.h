#ifndef WTC_PROGRESS_H
#define WTC_PROGRESS_H

// What an operation guarantees about progress, as a level and a bound
// (docs/progress.md, decisions/0004).
//
// ProgressOpInterface answered two questions once: can the operation's
// algorithm wait for another thread, and may it call the allocator. That
// is enough for a function declared lock-free and not enough for anything
// else. A wait-free path needs the number of its own steps, which depends
// on how many threads use the object; a real-time thread needs to know
// whether a wait is for data or for another thread's operation in
// progress; a latency-critical path needs the worst case, not the
// amortised one, and the effects that happen once per thread or once per
// process. Each of those is a field here, kept apart because each fails a
// different requirement:
//
//   level      blocking (deadlock-free or starvation-free), obstruction-free,
//              lock-free or wait-free, in the taxonomy of "On the nature of
//              progress" (OPODIS 2011);
//   partial    a dual operation ("Nonblocking concurrent data structures
//              with condition synchronization", DISC 2004): it waits for
//              data, and its level is that of the request and the
//              follow-up, not of the wait;
//   bound      for a wait-free operation, its own steps in the worst case,
//              as a polynomial in the thread counts; `amortised` where the
//              worst case and the amortised one differ;
//   alloc      whether it calls the allocator: never, the first time a
//              thread uses the object, once every K operations, or on any;
//   kernel     whether it enters the kernel: a futex wake, a futex wait;
//   firstUse   what happens once per thread or once per process: a
//              per-thread record taken on the thread's first operation
//              (which may call the allocator), a static-initialisation
//              guard;
//   element    which of the element's special members it runs: its copy
//              or move constructor on the producer's side, its
//              move-assignment and destructor on the consumer's;
//   reason     in words, why it may block or allocate, for a diagnostic.
//
// A step is one access to shared memory or one pause of a bounded wait.
// Steps are not time: a page fault, an interrupt or a cache miss is not
// in the model, and a bound is not a latency.

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace mlir::wtc {

// How many threads something stands for: one, a number the program fixes
// at compile time (a spawn in `for (i = 0; i < 4; ++i)`), a number it
// fixes before the threads start but that this module cannot read (the
// bound of the spawning loop, a variable: `sym` names it), or no number at
// all. Arithmetic is closed: anything with an unbounded operand is
// unbounded, a sum or product of constants is a constant, and the rest is
// symbolic, with `sym` the expression in words.
struct Count {
    enum class Kind : uint8_t { One, Const, Symbolic, Unbounded };
    Kind kind = Kind::Unbounded;
    uint64_t k = 0;   // One: 1; Const: the number
    std::string sym;  // Symbolic: the expression

    static Count one() { return {Kind::One, 1, {}}; }
    static Count constant(uint64_t k) { return k == 1 ? one() : Count{Kind::Const, k, {}}; }
    static Count symbolic(std::string s) { return {Kind::Symbolic, 0, std::move(s)}; }
    static Count unbounded() { return {}; }
    static Count zero() { return {Kind::Const, 0, {}}; }

    // The number, when there is one.
    std::optional<uint64_t> value() const {
        if (kind == Kind::One || kind == Kind::Const) return k;
        return std::nullopt;
    }
    bool isOne() const { return kind == Kind::One; }
    bool isUnbounded() const { return kind == Kind::Unbounded; }
    // At most one thread: one, or a constant zero or one.
    bool atMostOne() const { return value() && *value() <= 1; }

    // "1", "4", "N", "unbounded"; parse reads it back.
    std::string str() const;
    static Count parse(llvm::StringRef text);

    friend Count operator+(const Count &a, const Count &b);
    friend Count operator*(const Count &a, const Count &b);
    bool operator==(const Count &o) const { return kind == o.kind && k == o.k && sym == o.sym; }
};

// The counts an operation's guarantee depends on: the producers and the
// consumers of its object, and the threads that hold a record of the
// hazard-pointer domain (every thread that ever touched a queue that uses
// one, which is the program's thread count).
struct ThreadCounts {
    Count producers = Count::unbounded();
    Count consumers = Count::unbounded();
    Count domain = Count::unbounded();
    // What every check may assume: nothing.
    static ThreadCounts unknown() { return {}; }
};

// The guarantee, weakest first. Blocking comes in the two forms of the
// taxonomy: deadlock-free (some thread completes if none stops; a mutex)
// and starvation-free (every thread does; a wait for one operation in
// progress to finish).
enum class Level : uint8_t { blocking_df, blocking_sf, obstruction_free, lock_free, wait_free };
llvm::StringRef stringifyLevel(Level l);
// In words, for a remark: "blocking", "lock-free", "wait-free".
llvm::StringRef describeLevel(Level l);
std::optional<Level> parseLevel(llvm::StringRef text);

// One term of a bound: coeff · np^p · nc^c · n^d, where np, nc and n are
// the producers, the consumers and the hazard domain. A term may carry a
// name for what it counts ("slip pauses"), which a remark prints.
struct Term {
    uint64_t coeff = 0;
    uint8_t np = 0, nc = 0, n = 0;
    std::string name;
    bool sameMonomial(const Term &o) const { return np == o.np && nc == o.nc && n == o.n && name == o.name; }
};

// A bound on an operation's own steps.
//
//   Poly     a polynomial in the counts, a sum of terms (no terms: zero);
//   Finite   wait-free, but no closed form is known or could be read: an
//            input-dependent loop over local data, wCQ's slow path, whose
//            paper proves convergence and gives no number;
//   History  finite in each run but growing with the history of the
//            object, not bounded in the counts (the mpsc pop's in-order
//            step over cells taken out of order);
//   None     no bound: the operation is not wait-free.
//
// `note` says, for anything but a polynomial, why it is not one.
struct Bound {
    enum class Kind : uint8_t { Poly, Finite, History, None };
    Kind kind = Kind::None;
    llvm::SmallVector<Term, 4> terms;
    std::string note;

    static Bound zero() { return {Kind::Poly, {}, {}}; }
    static Bound constant(uint64_t c, llvm::StringRef name = {});
    static Bound poly(std::initializer_list<Term> terms);
    static Bound finite(std::string note) { return {Kind::Finite, {}, std::move(note)}; }
    static Bound history(std::string note) { return {Kind::History, {}, std::move(note)}; }
    static Bound none() { return {}; }

    bool isPoly() const { return kind == Kind::Poly; }
    // Whether the bound mentions np, nc or n.
    bool dependsOnCounts() const;

    // The bound's value under the counts, when it is a polynomial and every
    // count it mentions has a number.
    std::optional<uint64_t> instantiate(const ThreadCounts &counts) const;
    // The bound with every count that has a number put in: a term whose
    // counts all have one becomes a constant, the rest keep their
    // variables. Two terms of different objects then add: np stands for
    // the largest count among them.
    Bound substitute(const ThreadCounts &counts) const;

    // Sequence (the steps add), alternative (the larger, termwise: every
    // term is non-negative, so the termwise maximum bounds both), and
    // repetition (a loop run `trips` times).
    Bound plus(const Bound &o) const;
    Bound max(const Bound &o) const;
    Bound times(const Count &trips, llvm::StringRef what = {}) const;

    // "3 + 96 slip pauses", "4·np² + 4·np + 1", "finite (...)".
    std::string str() const;
};

// Whether an operation may call the allocator.
struct Alloc {
    enum class Kind : uint8_t { never, first_use, amortised, always };
    Kind kind = Kind::never;
    uint64_t every = 0;  // amortised: once every this many operations
    static Alloc never() { return {}; }
    static Alloc firstUse() { return {Kind::first_use, 0}; }
    static Alloc amortised(uint64_t k) { return {Kind::amortised, k}; }
    static Alloc always() { return {Kind::always, 0}; }
    std::string str() const;
};

// Whether an operation enters the kernel, and how.
enum class Kernel : uint8_t { none, futex_wake, futex_wait };
llvm::StringRef stringifyKernel(Kernel k);

// What happens the first time a thread, or the process, runs the
// operation. Flags.
enum FirstUse : uint8_t {
    first_use_none = 0,
    // A per-thread record taken on the thread's first operation: the
    // runtime's hazard record (runtime/hazard.hpp, tied_record), one
    // compare-and-swap or fetch-and-add and a pthread_setspecific, which
    // calls the allocator unless the key is among the C library's first 32
    // and fewer than 256 threads hold records, and for the wait-free ring of
    // indices its thread slot.
    first_use_tls = 1,
    // A function-local static's guard, and whatever its initialiser does;
    // the program's own (no operation of this runtime takes one).
    first_use_guard = 2,
};
std::string describeFirstUse(unsigned flags);

// Which of the element's special members an operation runs. Flags.
enum ElementCode : uint8_t {
    element_none = 0,
    element_copy_ctor = 1,    // producer: push(const T&)
    element_move_ctor = 2,    // producer: push(T&&); consumer: the shared_ptr form
    element_move_assign = 4,  // consumer: try_pop(T&), pop_wait(T&)
    element_dtor = 8,         // consumer: the element left in the cell
};
std::string describeElementCode(unsigned flags);

struct ProgressSpec {
    Level level = Level::blocking_df;
    bool partial = false;
    Bound bound;      // wait-free only; None otherwise
    Bound amortised;  // where it differs from `bound`; None when it does not
    Alloc alloc;
    Kernel kernel = Kernel::none;
    unsigned firstUse = first_use_none;
    unsigned element = element_none;
    std::string reason;

    // Whether progress depends on the scheduler: blocking needs a fair
    // one and no thread that stops, obstruction-freedom a thread that
    // eventually runs alone.
    bool dependent() const { return level < Level::lock_free; }

    // The old two-valued answers, as ProgressOpInterface gave them before
    // the specification: a waiting operation blocks whatever the level of
    // its request, and anything short of lock-free blocks.
    bool blocks() const { return partial || level < Level::lock_free; }
    bool allocates() const { return alloc.kind != Alloc::Kind::never; }

    // One line, for the table and for remarks:
    // "wait-free, ≤ 3 + 96 slip pauses; allocates: never; ..."
    std::string str(const ThreadCounts *counts = nullptr) const;
};

// Every form a wtc operation takes, for the table.
enum class OpForm : uint8_t {
    push,                 // queue_push, unbounded
    push_deferred,        // queue_push_deferred
    batch_begin,          // queue_batch_begin
    batch_end,            // queue_batch_end
    pop_shared,           // queue_pop with a shared_ptr result
    pop_transfer,         // queue_pop transfer (try_pop(T&))
    pop_wait,             // queue_pop wait
    pop_wait_for,         // queue_pop wait with a timeout
    bounded_try_push,     // queue_push bounded, with a result
    bounded_push_wait,    // queue_push bounded wait
    bounded_push_overrun, // queue_push bounded overrun
    bounded_push_batch,   // queue_push bounded batch
    bounded_pop_shared,   // queue_pop bounded with a shared_ptr result
    bounded_pop_transfer, // queue_pop bounded transfer
    bounded_pop_wait,     // queue_pop bounded wait
    bounded_pop_wait_for, // queue_pop bounded wait with a timeout
    bounded_pop_batch,    // queue_pop bounded batch
    lock,
    unlock,
    lock_batch_begin,
    lock_batch_step,
    lock_batch_end,
    delegate,
};
llvm::StringRef stringifyOpForm(OpForm f);
// Whether the form acts on a queue (and so depends on its implementation),
// and whether it is the producer's side.
bool isQueueForm(OpForm f);
bool isBoundedForm(OpForm f);

} // namespace mlir::wtc

#endif // WTC_PROGRESS_H
