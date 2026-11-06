#include "WTC/Transforms/ClosedWorld.h"
#include "WTC/Transforms/EscapeAnalysis.h"
#include "WTC/Transforms/LiftNames.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"

#include "mlir/IR/SymbolTable.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace mlir;
using namespace mlir::wtc;

LogicalResult ClosedWorld::init(ModuleOp m, const ClosedWorldOptions &o) {
    module = m;
    options = o;
    auto mainFn = module.lookupSymbol<cir::FuncOp>("main");
    main = mainFn && !mainFn.getBody().empty();
    raw.clear();
    demangled.clear();
    defined.clear();
    if (!options.definedElsewhereFile.empty()) {
        auto buf = llvm::MemoryBuffer::getFile(options.definedElsewhereFile);
        if (!buf) {
            module.emitError() << "wtc: cannot read the symbols defined elsewhere from "
                               << options.definedElsewhereFile;
            return failure();
        }
        llvm::SmallVector<StringRef, 64> lines;
        (*buf)->getBuffer().split(lines, '\n', -1, false);
        for (StringRef l : lines)
            if (!(l = l.trim()).empty()) defined.insert(l);
    }
    if (options.namedElsewhereFile.empty()) return success();
    auto buf = llvm::MemoryBuffer::getFile(options.namedElsewhereFile);
    if (!buf) {
        module.emitError() << "wtc: cannot read the symbols named elsewhere from " << options.namedElsewhereFile;
        return failure();
    }
    llvm::SmallVector<StringRef, 64> lines;
    (*buf)->getBuffer().split(lines, '\n', -1, false);
    for (StringRef l : lines) {
        if ((l = l.trim()).empty()) continue;
        raw.insert(l);
        demangled.push_back(squashSpaces(llvm::demangle(l.str())));
    }
    return success();
}

bool ClosedWorld::namesFunction(cir::FuncOp f) const {
    return llvm::is_contained(demangled, squashSpaces(llvm::demangle(f.getSymName().str())));
}

std::string ClosedWorld::mentioning(StringRef cls) const {
    const std::string want = squashSpaces(cls);
    for (const std::string &s : demangled)
        if (mentions(s, want)) return s;
    return "";
}

namespace {

// What a value that may point to an object of `rec` may point to as well:
// the object, a record or array that holds it, void, a byte.
bool byteOrHolder(Type t, cir::RecordType rec) {
    if (isa<cir::VoidType>(t)) return true;
    if (auto i = dyn_cast<cir::IntType>(t); i && i.getWidth() == 8) return true;
    llvm::SmallVector<Type, 8> work{t};
    llvm::DenseSet<Type> seen;
    while (!work.empty()) {
        Type x = work.pop_back_val();
        if (x == Type(rec)) return true;
        if (!seen.insert(x).second) continue;
        if (auto a = dyn_cast<cir::ArrayType>(x)) work.push_back(a.getElementType());
        else if (auto r = dyn_cast<cir::RecordType>(x); r && !r.isIncomplete())
            for (Type m : r.getMembers()) work.push_back(m);
    }
    return false;
}

}  // namespace

// (f) The class is this module's alone. Lifting changes what its member
// functions do with the object's storage; a copy of them compiled in
// another unit, as written, would read the same storage as the container
// it no longer holds, and the linker keeps one copy of an inline member
// function for the whole program, which may be either. So the class lifts
// only when the module is the whole program as far as the class goes: it
// has main, it is not compiled to be linked with files the pass does not
// see, none of the files it is linked with names the class (a member
// function, or a function that takes or returns the class), and every
// member function of the class this module declares has its body here.
//
// Symbols alone cannot show that a linked file leaves the class alone: its
// copy of an inline member function may be inlined where it is called,
// leaving no symbol that names the class. So, with any file linked, no
// object of the class may be reached by that file's code: the escape
// analysis (EscapeAnalysis.h) follows every value that may carry an
// object's address, through casts, integers, memory, calls and pointers
// to functions, to the roots that code can see.
void ClosedWorld::closeClass(cir::RecordType rec, const llvm::DenseSet<Operation *> &own, Type element,
                             Verdict &v) const {
    const std::string cls = recordName(rec).str();
    if (openWorld()) {
        v.refuse("(f) the unit is compiled to be linked with files the pass does not see, which may use " + cls +
                 " as written");
        return;
    }
    if (!hasMain()) {
        v.refuse("(f) the module has no main: the rest of the program, elsewhere, may use " + cls + " as written");
        return;
    }
    for (cir::FuncOp f : module.getOps<cir::FuncOp>())
        if (f.getBody().empty() && isMemberOf(f, rec))
            v.refuse("(f) " + shortName(f) + " has no body here: another unit defines it");
    if (std::string s = mentioning(cls); !s.empty()) {
        v.refuse("(f) a file linked with this one names " + cls + " (" + s + ")");
        return;
    }
    // The escape analysis runs whether or not a file is linked: with none,
    // the code the pass does not see is the system's libraries, and an
    // object they may hand back, or keep where the program reads it as
    // another type, is not confined either.
    EscapeQuery q;
    q.own = own;
    q.self = [rec](cir::FuncOp g) -> std::optional<unsigned> {
        const unsigned i = g.getNumArguments() >= 2 && g.getArgAttr(0, "llvm.sret") ? 1 : 0;
        auto p = i < g.getNumArguments() ? dyn_cast<cir::PointerType>(g.getArgumentTypes()[i]) : cir::PointerType();
        if (!p || p.getPointee() != Type(rec)) return std::nullopt;
        return i;
    };
    q.namesSymbol = [this](StringRef sym) { return names(sym); };
    q.namesFunction = [this](cir::FuncOp f) { return namesFunction(f); };
    q.definedElsewhere = [this](StringRef sym) { return defines(sym); };
    q.nameOf = [](cir::FuncOp f) { return shortName(f); };
    q.allowedPointee = [rec](Type t) { return byteOrHolder(t, rec); };
    q.elementType = element;
    q.linked = linked();
    EscapeResult r = escapes(module, q);
    if (r.escape) v.refuse("(f) an object of " + cls + " may be reached by code the pass does not see: " + *r.escape);
    if (r.reread) v.refuse("(a) " + *r.reread + ", and reads " + cls + " as that type");
    // A pointer to a member of the class reaches the member's storage
    // with no member step the confinement sees.
    std::string mp;
    module.walk([&](Operation *op) {
        if (!mp.empty()) return;
        auto f = op->getParentOfType<cir::FuncOp>();
        if (f && own.contains(f)) return;
        for (Value x : op->getResults())
            if (auto dm = dyn_cast<cir::DataMemberType>(x.getType()); dm && dm.getClassTy() == rec)
                mp = (f ? shortName(f) : std::string("a global's initialiser"));
    });
    if (!mp.empty())
        v.refuse("(a) " + mp + " takes a pointer to a member of " + cls + ", which reaches its storage as written");
}

namespace {

// Where a pointer to the struct may be kept: a local of pointer type whose
// address goes only to loads and stores, or a global of that type, or an
// array of them, whose address goes only to loads and stores, through
// element steps.
bool pointerCell(Value addr, Type ptrTy) {
    std::function<bool(Value, unsigned)> onlyLoadsAndStores = [&](Value a, unsigned depth) {
        if (depth > 4) return false;
        for (OpOperand &u : a.getUses()) {
            Operation *o = u.getOwner();
            if (auto ld = dyn_cast<cir::LoadOp>(o); ld && ld.getAddr() == a) continue;
            if (auto st = dyn_cast<cir::StoreOp>(o); st && st.getAddr() == a) continue;
            if (isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(o)) continue;
            if (auto ps = dyn_cast<cir::PtrStrideOp>(o); ps && ps.getBase() == a) {
                if (!onlyLoadsAndStores(ps.getResult(), depth + 1)) return false;
                continue;
            }
            if (auto cast = dyn_cast<cir::CastOp>(o); cast && cast.getKind() == cir::CastKind::array_to_ptrdecay) {
                if (!onlyLoadsAndStores(cast.getResult(), depth + 1)) return false;
                continue;
            }
            return false;
        }
        return true;
    };
    Value root = addr;
    while (true) {
        if (auto ps = root.getDefiningOp<cir::PtrStrideOp>()) root = ps.getBase();
        else if (auto cast = root.getDefiningOp<cir::CastOp>(); cast && cast.getKind() == cir::CastKind::array_to_ptrdecay)
            root = cast.getSrc();
        else break;
    }
    auto holds = [&](Type t) {
        while (auto a = dyn_cast<cir::ArrayType>(t)) t = a.getElementType();
        return t == ptrTy;
    };
    if (auto a = root.getDefiningOp<cir::AllocaOp>())
        return holds(a.getAllocaType()) && onlyLoadsAndStores(a.getResult(), 0);
    if (auto g = root.getDefiningOp<cir::GetGlobalOp>()) {
        auto mod = g->getParentOfType<ModuleOp>();
        auto glob = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(g, g.getNameAttr());
        if (!glob || !holds(glob.getSymType())) return false;
        // Every use of the global, in every function: loads and stores.
        bool ok = true;
        mod.walk([&](cir::GetGlobalOp other) {
            if (ok && other.getName() == g.getName()) ok = onlyLoadsAndStores(other.getResult(), 0);
        });
        if (auto uses = SymbolTable::getSymbolUses(glob, mod))
            for (const SymbolTable::SymbolUse &u : *uses)
                if (!isa<cir::GetGlobalOp>(u.getUser())) ok = false;
        return ok;
    }
    return false;
}

}  // namespace

bool ClosedWorld::confineStruct(cir::RecordType rec, const llvm::DenseSet<Operation *> &ops,
                                const llvm::DenseSet<Operation *> &replaced,
                                const std::function<std::optional<unsigned>(cir::FuncOp)> &self, Type element,
                                const std::function<std::string(unsigned)> &field, Verdict &v) const {
    auto no = [&](const Twine &w) {
        v.refuse(w);
        return false;
    };
    // The struct's members and pointers to it, outside its functions.
    Type ptrTy = cir::PointerType::get(rec);
    std::string bad;
    module.walk([&](Operation *op) {
        if (!bad.empty()) return;
        auto fn = op->getParentOfType<cir::FuncOp>();
        if (fn && ops.count(fn)) return;
        if (auto gm = dyn_cast<cir::GetMemberOp>(op))
            if (gm.getAddr().getType() == ptrTy) {
                bad = "(a) " + (fn ? fn.getSymName().str() : std::string("a global")) + " touches " +
                      field(static_cast<unsigned>(gm.getIndex()));
                return;
            }
        for (Value r : op->getResults()) {
            if (r.getType() == Type(rec)) {
                bad = "(a) " + (fn ? fn.getSymName().str() : std::string("a global")) + " copies the struct";
                return;
            }
            if (r.getType() != ptrTy) continue;
            for (OpOperand &u : r.getUses()) {
                Operation *user = u.getOwner();
                if (auto gm = dyn_cast<cir::GetMemberOp>(user)) {
                    bad = "(a) " + (fn ? fn.getSymName().str() : std::string("a global")) + " touches " +
                          field(static_cast<unsigned>(gm.getIndex()));
                    return;
                }
                // The pointer stored: only into a local slot or a global
                // that holds pointers to the struct and nothing else, whose
                // loads are pointers to the struct and are checked here as
                // every other one is. Stored anywhere else (a member of
                // another struct, through a pointer), it reaches code that
                // the walk does not follow.
                if (auto st = dyn_cast<cir::StoreOp>(user); st && st.getValue() == r) {
                    if (pointerCell(st.getAddr(), ptrTy)) continue;
                    bad = "(a) " + (fn ? fn.getSymName().str() : std::string("a global")) +
                          " stores a pointer to the struct in memory the pass does not follow";
                    return;
                }
                if (isa<cir::PtrStrideOp, cir::CmpOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(user)) continue;
                if (auto call = dyn_cast<cir::CallOp>(user)) {
                    cir::FuncOp callee = lookupCallee(call);
                    if (callee && ops.count(callee)) continue;
                    if (callee && callee.getSymName() == "free") continue;
                }
                if (auto cast = dyn_cast<cir::CastOp>(user)) {
                    if (cast.getKind() == cir::CastKind::ptr_to_bool) continue;
                    bool toFree = llvm::all_of(cast->getUses(), [](OpOperand &cu) {
                        auto call = dyn_cast<cir::CallOp>(cu.getOwner());
                        cir::FuncOp callee = call ? lookupCallee(call) : cir::FuncOp();
                        return callee && callee.getSymName() == "free";
                    });
                    if (toFree) continue;
                }
                bad = "(a) " + (fn ? fn.getSymName().str() : std::string("a global")) + " passes a pointer to the struct to " +
                      user->getName().getStringRef().str() +
                      (isa<cir::CallOp>(user) && lookupCallee(cast<cir::CallOp>(user))
                           ? " " + lookupCallee(cast<cir::CallOp>(user)).getSymName().str()
                           : std::string());
                return;
            }
        }
    });
    if (!bad.empty()) return no(bad);
    // A struct, or array, that holds the queue by value, at any depth
    // (`struct app { int id; struct chan q; }`), other than the queue's
    // own struct: whatever reaches it reaches the queue. Its objects are
    // confined as the queue's are: not copied or written whole, and a
    // pointer to one goes only to member and element steps (whose pointers
    // to the queue the walk above checks), comparisons, loads and stores
    // of its own type, functions with a body here, and free.
    std::function<bool(Type, unsigned)> holds = [&](Type t, unsigned depth) -> bool {
        if (depth > 32) return true;  // too deep to tell: as if it did
        if (t == Type(rec)) return depth > 0;
        if (auto a = dyn_cast<cir::ArrayType>(t)) return holds(a.getElementType(), depth + 1);
        if (auto r = dyn_cast<cir::RecordType>(t); r && !r.isIncomplete())
            for (Type m : r.getMembers())
                if (m == Type(rec) || holds(m, depth + 1)) return true;
        return false;
    };
    auto toHolder = [&](Type t) {
        auto p = dyn_cast<cir::PointerType>(t);
        return p && holds(p.getPointee(), 0);
    };
    auto freed = [](Value x) {
        return llvm::all_of(x.getUses(), [](OpOperand &cu) {
            auto call = dyn_cast<cir::CallOp>(cu.getOwner());
            cir::FuncOp callee = call ? lookupCallee(call) : cir::FuncOp();
            return callee && callee.getSymName() == "free";
        });
    };
    module.walk([&](Operation *op) {
        if (!bad.empty()) return;
        auto fn = op->getParentOfType<cir::FuncOp>();
        if (fn && ops.count(fn)) return;
        const std::string at = fn ? fn.getSymName().str() : std::string("a global");
        // A union that holds the queue shares its storage with its other
        // members: a step into one of those reaches the queue's bytes as
        // another type.
        if (auto gm = dyn_cast<cir::GetMemberOp>(op)) {
            auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
            auto u = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
            if (u && u.isUnion() && !u.isIncomplete() && holds(u, 0) && gm.getIndex() < u.getMembers().size()) {
                Type m = u.getMembers()[gm.getIndex()];
                if (m != Type(rec) && !holds(m, 0)) {
                    bad = "(a) " + at + " reads a union that holds the queue through another of its members";
                    return;
                }
            }
        }
        for (Value r : op->getResults())
            if (holds(r.getType(), 0)) {
                bad = "(a) " + at + " copies a struct that holds the queue";
                return;
            }
        if (auto cp = dyn_cast<cir::CopyOp>(op); cp && holds(cp.getDst().getType().getPointee(), 0)) {
            bad = "(a) " + at + " copies a struct that holds the queue";
            return;
        }
        if (auto st = dyn_cast<cir::StoreOp>(op)) {
            if (holds(st.getValue().getType(), 0)) {
                bad = "(a) " + at + " writes a struct that holds the queue whole";
                return;
            }
            if (toHolder(st.getValue().getType()) && !pointerCell(st.getAddr(), st.getValue().getType())) {
                bad = "(a) " + at + " stores a pointer to a struct that holds the queue in memory the pass does not follow";
                return;
            }
        }
        if (auto cast = dyn_cast<cir::CastOp>(op); cast && toHolder(cast.getSrc().getType()) &&
                                                   cast.getKind() != cir::CastKind::ptr_to_bool &&
                                                   cast.getResult().getType() != ptrTy &&
                                                   !toHolder(cast.getResult().getType()) && !freed(cast.getResult())) {
            bad = "(a) " + at + " converts a pointer to a struct that holds the queue to another type";
            return;
        }
        if (auto call = dyn_cast<cir::CallOp>(op)) {
            cir::FuncOp callee = call.isIndirect() ? cir::FuncOp() : lookupCallee(call);
            if (callee && callee.getSymName() == "free") return;
            if ((!callee || callee.isDeclaration()) &&
                llvm::any_of(call.getArgOperands(), [&](Value a) { return toHolder(a.getType()); }))
                bad = "(a) " + at + " passes a pointer to a struct that holds the queue to " +
                      (callee ? callee.getSymName().str() + ", which has no body here" : std::string("an indirect call"));
        }
    });
    if (!bad.empty()) return no(bad);
    // Nothing another unit sees: the struct's functions by name, and, with
    // any file linked, no value that may point to a queue reaching that
    // file's code (EscapeAnalysis.h), whatever its type on the way.
    for (Operation *f : ops)
        if (names(cast<cir::FuncOp>(f).getSymName()))
            return no("(a) " + cast<cir::FuncOp>(f).getSymName() + " is named by another file linked with this one");
    EscapeQuery q;
    q.own = replaced;
    q.self = self;
    q.namesSymbol = [this](StringRef sym) { return names(sym); };
    q.namesFunction = [this](cir::FuncOp f) { return names(f.getSymName()); };
    q.definedElsewhere = [this](StringRef sym) { return defines(sym); };
    q.nameOf = [](cir::FuncOp f) { return f.getSymName().str(); };
    q.allowedPointee = [rec](Type t) { return byteOrHolder(t, rec); };
    q.elementType = element;
    q.linked = linked();
    EscapeResult r = escapes(module, q);
    if (r.escape) return no("(a) a queue may be reached by code the pass does not see: " + *r.escape);
    if (r.reread) return no("(a) " + *r.reread + ", and reads a queue as that type");
    return true;
}
