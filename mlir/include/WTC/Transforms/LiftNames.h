#ifndef WTC_TRANSFORMS_LIFTNAMES_H
#define WTC_TRANSFORMS_LIFTNAMES_H

// Names as the lifting passes read them: a C++ function's mangled name in
// parts, a record's name, and the short forms the remarks use.

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Demangle/Demangle.h"

#include <cstdlib>
#include <string>

namespace mlir::wtc {

// A function's name in parts, from the Itanium demangler.
struct Name {
    std::string base, context, params;
    bool ctor = false, dtor = false, ok = false;
};

inline Name nameOf(llvm::StringRef mangled) {
    Name n;
    if (!mangled.starts_with("_Z")) {
        n.base = mangled.str();
        return n;
    }
    const std::string s = mangled.str();
    llvm::ItaniumPartialDemangler d;
    if (d.partialDemangle(s.c_str()) || !d.isFunction()) return n;
    auto take = [](char *c) {
        std::string out = c ? c : "";
        std::free(c);
        return out;
    };
    size_t len = 0;
    n.base = take(d.getFunctionBaseName(nullptr, &len));
    n.context = take(d.getFunctionDeclContextName(nullptr, &len));
    n.params = take(d.getFunctionParameters(nullptr, &len));
    n.ctor = d.isCtorOrDtor() && !llvm::StringRef(n.base).starts_with("~");
    n.dtor = d.isCtorOrDtor() && llvm::StringRef(n.base).starts_with("~");
    n.ok = true;
    return n;
}

inline std::string pretty(llvm::StringRef mangled) { return llvm::demangle(mangled.str()); }

inline llvm::StringRef recordName(Type t) {
    auto r = dyn_cast_or_null<cir::RecordType>(t);
    return r && r.getName() ? r.getName().getValue() : llvm::StringRef();
}

// Where a member function's name sits inside the demangled one, for remarks
// that must stay short: "shared_queue<int>::push".
inline std::string shortName(cir::FuncOp f) {
    Name n = nameOf(f.getSymName());
    if (!n.ok) return f.getSymName().str();
    return n.context.empty() ? n.base : n.context + "::" + n.base;
}

inline std::string squashSpaces(llvm::StringRef x) {
    std::string out;
    for (char ch : x)
        if (ch != ' ') out += ch;
    return out;
}

inline bool isMemberOf(cir::FuncOp f, cir::RecordType rec) {
    Name n = nameOf(f.getSymName());
    return n.ok && squashSpaces(n.context) == squashSpaces(recordName(rec));
}

// Whether a demangled name, spaces removed, mentions the class: its name
// as a whole word, as the scope of a member ("LQ<int>::pending()") or in a
// parameter ("report(LQ<int>&)").
inline bool mentions(llvm::StringRef name, llvm::StringRef cls) {
    auto word = [](char ch) { return llvm::isAlnum(ch) || ch == '_'; };
    for (size_t at = name.find(cls); at != llvm::StringRef::npos; at = name.find(cls, at + 1)) {
        const size_t end = at + cls.size();
        if ((at == 0 || !word(name[at - 1])) && (end == name.size() || !word(name[end]))) return true;
    }
    return false;
}

}  // namespace mlir::wtc

#endif
