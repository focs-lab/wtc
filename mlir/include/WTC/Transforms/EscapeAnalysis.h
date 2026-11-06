#ifndef WTC_TRANSFORMS_ESCAPEANALYSIS_H
#define WTC_TRANSFORMS_ESCAPEANALYSIS_H

// Whether an object can be reached by code the pass does not see: the core
// of the closed world (ClosedWorld.h, docs/lifting.md "A closed world").
//
// The analysis follows values, not types. It is a unification-based
// points-to analysis over the whole module (Steensgaard's), flow- and
// field-insensitive: every value is a node; an assignment, a cast of any
// kind (an integer made from a pointer included), a member or element
// step, arithmetic, a call's argument and its callee's parameter, a
// return and the call's result join two nodes into one class; a load joins
// its result with what its address's class points to, a store its value,
// a copy the two contents. A class stands for every value that may carry
// the address of the same objects, so a pointer made an integer and back,
// a pointer kept in a slot, a field, a global or a lambda's captures, and
// a function pointer that returns it are all one class with the object.
//
// The code the pass does not see is one more class, the outside, which
// points to itself: whatever reaches it may be read, kept and handed on
// by that code. It runs whether or not a file is linked: with none, the
// outside is the system's libraries, which may hand back what they are
// handed. The roots join it:
//
//   - a global another file names (named-elsewhere), one only declared
//     here (but the C++ ABI's vtables and type_info, which hold no object
//     of the program's), one in a section of its own, and llvm.used;
//   - the parameters and result of a function another file names, or of
//     the aliasee of an alias it names;
//   - the arguments and the result of a call of a function with no body
//     here. Deny-by-default: a short, audited list of the C library's and
//     libstdc++'s functions says what each does with its pointers
//     (allocation, deallocation, mutexes and condition variables, string
//     and memory functions with their copies and returned arguments,
//     formatted output and input with the conversions of a literal format,
//     reads and writes of files, pthread_join's result, the registration
//     of a callback whose argument goes to the callback's parameter), and
//     a function the list does not name, or one a linked file defines
//     under a name on it, may keep and hand on what it is handed;
//   - an indirect call whose callee may be the outside's, or only a
//     virtual function of a class whose vtable is only declared here;
//   - a function whose address reaches the outside: its parameters and its
//     result;
//   - inline assembly's operands, and an exception thrown, whose handler
//     may be another file's.
//
// What the program writes to files and reads back is one more class; with
// a file linked it is the outside, whose code may read the files too. A
// va_list holds the variadic arguments of the function that started it.
// An object's vptr is a field of its own, apart from the rest of the
// object. The shim's own functions (namespace wtc) are one class: what
// they are handed they may hand back from any of them.
//
// The candidate's own functions (`own`: the member functions of a C++
// class, the functions of a C struct) are left out: the rewrite replaces
// their bodies. A call of one hands it the object, and what reaches the
// object there is what the call is handed as the object; their other
// arguments and results are joined, the elements going in and out, by
// value or through what an argument that points to an element points to.
//
// The object escapes when the class of a value a call of its own
// functions is handed as the object is the outside. The roots are joined
// one at a time, so that the reason given is the root that made it so.
// Apart from escaping, the module itself may read the object as another
// type: a cast that makes a value typed as the object, or one in the
// object's class in the program's own code (not the system's headers),
// a pointer to another type than the object's, a record that holds it,
// void or a byte, is reported (a pointer to the first member, offsetof
// arithmetic from another member).

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <optional>
#include <string>

namespace mlir::wtc {

struct EscapeQuery {
    // The candidate's own functions.
    llvm::DenseSet<Operation *> own;
    // Which argument of a call of an own function is the object.
    std::function<std::optional<unsigned>(cir::FuncOp)> self;
    // Whether the files linked with the module name a symbol, and a
    // function (C++ names are compared demangled).
    std::function<bool(llvm::StringRef)> namesSymbol;
    std::function<bool(cir::FuncOp)> namesFunction;
    // Whether a linked file defines a symbol: a function of a library's
    // name there is the program's, and no library model applies to it.
    std::function<bool(llvm::StringRef)> definedElsewhere;
    // A function's name in a remark.
    std::function<std::string(cir::FuncOp)> nameOf;
    // The types a value that may point to the object may be cast to point
    // to: its own, a record that holds it, void, a byte.
    std::function<bool(mlir::Type)> allowedPointee;
    // The type of the elements the own functions take and hand back, when
    // known: an argument that points to one passes the element through
    // what it points to, any other passes it as itself.
    mlir::Type elementType;
    // Whether any file is linked with the module: what it writes to files
    // that file's code may read.
    bool linked = false;
};

struct EscapeResult {
    // Why the object may be reached by code the pass does not see.
    std::optional<std::string> escape;
    // Where the module itself reads it as another type.
    std::optional<std::string> reread;
};

// The reason the object may be reached by code the pass does not see, or
// nothing when it cannot. "<where> passes a value that may point to it to
// <g>, which has no body here", and the like.
EscapeResult escapes(ModuleOp module, const EscapeQuery &q);

}  // namespace mlir::wtc

#endif
