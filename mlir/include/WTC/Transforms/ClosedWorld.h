#ifndef WTC_TRANSFORMS_CLOSEDWORLD_H
#define WTC_TRANSFORMS_CLOSEDWORLD_H

// What the rest of the program may see of a module, for the lifting passes
// (docs/lifting.md, "The contract"). Lifting changes what a class's member
// functions, or a struct's functions, do with the object's storage; code
// the pass does not see, compiled as written, would read that storage as
// the container or the ring it no longer holds. So a candidate lifts only
// where the module is the whole program as far as the object goes:
//
//   - the module has main and is not compiled to be linked with files the
//     pass does not see (open-world, what wtc-cc -c passes);
//   - the files linked with it (named-elsewhere, the symbols wtc-cc lists
//     with nm from the object files, archives and shared objects it links)
//     name neither the candidate's functions nor its objects;
//   - and, with any file linked, the module's own code keeps the object
//     out of those files' reach: condition (f) for a C++ class, part of
//     condition (a) for a C struct.
//
// The symbol list is read once per pass run, here, for both passes.

#include "WTC/Transforms/Refusal.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Support/LogicalResult.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

#include <functional>
#include <string>
#include <vector>

namespace mlir::wtc {

struct ClosedWorldOptions {
    // The module is linked later with files the pass does not see.
    bool openWorld = false;
    // A file listing, one per line, the symbols the files linked with the
    // module define or reference; empty when none is linked.
    std::string namedElsewhereFile;
};

class ClosedWorld {
public:
    // Fails, with an error on the module, when the symbol list cannot be
    // read: an unreadable list would let every class lift.
    LogicalResult init(ModuleOp module, const ClosedWorldOptions &options);

    bool openWorld() const { return options.openWorld; }
    bool hasMain() const { return main; }
    // Whether any file is linked with this one (a list was given, even an
    // empty one).
    bool linked() const { return !options.namedElsewhereFile.empty(); }
    // Whether the list names any symbol at all.
    bool anyNamed() const { return !demangled.empty(); }
    // Whether the linked files name this symbol, exactly.
    bool names(llvm::StringRef symbol) const { return raw.contains(symbol); }
    // Whether they name this function of the module, by its demangled
    // name with the spaces removed.
    bool namesFunction(cir::FuncOp f) const;
    // A symbol of theirs, demangled, that mentions the C++ class named
    // `cls`, or empty.
    std::string mentioning(llvm::StringRef cls) const;

    // Condition (f) for a C++ class: the reasons the module may not be the
    // whole program for objects of `rec`, recorded in `v`.
    void closeClass(cir::RecordType rec, Verdict &v) const;

    // Condition (a) for a C struct, outside its own functions (`own`, the
    // functions that take a pointer to it): its members touched, objects
    // copied, pointers to it or to what holds it handed where the pass does
    // not follow them, and what the linked files name. `field` names a
    // member of the struct, for the remarks. Returns false at the first
    // reason, which it records in `v`.
    bool confineStruct(cir::RecordType rec, const llvm::DenseSet<Operation *> &own,
                       const std::function<std::string(unsigned)> &field, Verdict &v) const;

private:
    mutable ModuleOp module;  // walked by the const checks
    ClosedWorldOptions options;
    bool main = false;
    llvm::StringSet<> raw;               // the symbols, as nm lists them
    std::vector<std::string> demangled;  // demangled, spaces removed
};

}  // namespace mlir::wtc

#endif
