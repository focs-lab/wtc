#include "WTC/Transforms/ClosedWorld.h"
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
// leaving no symbol that names the class. So, with any file linked, the
// object must also be out of its reach, by a rule on this module's own
// code: no global another file may name (not internal, or only declared
// here) holds the class or points to it, at any depth; no call hands
// something that holds or points to it to a function with no body here,
// to an indirect call or to a function a linked file names; no function
// of this module that a linked file names takes or returns such a thing;
// and no pointer to such a thing is converted to a type that is not one,
// which the pass could no longer follow (to `void *`, to an integer, to a
// base class), other than to free it.
void ClosedWorld::closeClass(cir::RecordType rec, Verdict &v) const {
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
    if (!linked()) return;

    // Whether a type is the class, holds it or points to something that
    // does, at any depth: a search of the types it is made of.
    llvm::DenseMap<Type, bool> memo;
    auto reaches = [&](Type root) {
        auto [it, fresh] = memo.try_emplace(root, false);
        if (!fresh) return it->second;
        llvm::SmallVector<Type, 16> work{root};
        llvm::DenseSet<Type> seen{root};
        bool found = false;
        while (!work.empty() && !found) {
            Type t = work.pop_back_val();
            if (t == Type(rec)) {
                found = true;
                break;
            }
            llvm::SmallVector<Type, 8> next;
            if (auto p = dyn_cast<cir::PointerType>(t)) next.push_back(p.getPointee());
            else if (auto a = dyn_cast<cir::ArrayType>(t)) next.push_back(a.getElementType());
            else if (auto r = dyn_cast<cir::RecordType>(t); r && !r.isIncomplete())
                next.append(r.getMembers().begin(), r.getMembers().end());
            for (Type n : next)
                if (seen.insert(n).second) work.push_back(n);
        }
        memo[root] = found;
        return found;
    };
    auto namedThere = [&](cir::FuncOp f) { return namesFunction(f); };
    auto where = [](Operation *op) {
        auto f = op->getParentOfType<cir::FuncOp>();
        return f ? shortName(f) : std::string("a global's initialiser");
    };
    auto deallocates = [](cir::FuncOp g) {
        StringRef n = g ? g.getSymName() : StringRef();
        return n.starts_with("_ZdlPv") || n.starts_with("_ZdaPv");
    };
    auto typeName = [](Type t) -> std::string {
        if (auto p = dyn_cast<cir::PointerType>(t)) {
            if (isa<cir::VoidType>(p.getPointee())) return "void *";
            if (StringRef n = recordName(p.getPointee()); !n.empty()) return (n + " *").str();
            return "a pointer of another type";
        }
        if (isa<cir::IntType>(t)) return "an integer";
        return "another type";
    };
    std::string bad;
    for (cir::GlobalOp g : module.getOps<cir::GlobalOp>())
        if ((!cir::isLocalLinkage(g.getLinkage()) || g.isDeclaration()) && reaches(g.getSymType())) {
            bad = "the global " + g.getSymName().str() + ", which a file linked with this one may name, holds or "
                  "points to " + cls;
            break;
        }
    for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
        if (!bad.empty()) break;
        if (f.getBody().empty() || !namedThere(f)) continue;
        auto types = f.getFunctionType();
        if (llvm::any_of(types.getInputs(), reaches) || llvm::any_of(types.getReturnTypes(), reaches))
            bad = "a file linked with this one names " + pretty(f.getSymName()) + ", which takes or returns " + cls +
                  " or a pointer to it";
    }
    if (bad.empty())
        module.walk([&](Operation *op) {
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                cir::FuncOp g = call.isIndirect() ? cir::FuncOp() : lookupCallee(call);
                const bool foreign = !g || g.getBody().empty() || namedThere(g);
                if (foreign && llvm::any_of(call.getArgOperands(), [&](Value x) { return reaches(x.getType()); })) {
                    bad = where(op) + " hands " + cls + ", or what holds or points to it, to " +
                          (g ? pretty(g.getSymName()) : std::string("an indirect call")) +
                          (!g ? std::string() : g.getBody().empty() ? ", which has no body here"
                                                                    : ", which a file linked with this one names");
                    return WalkResult::interrupt();
                }
            }
            Value from, to;
            if (auto cast = dyn_cast<cir::CastOp>(op); cast && cast.getKind() != cir::CastKind::ptr_to_bool) {
                from = cast.getSrc();
                to = cast.getResult();
            } else if (auto base = dyn_cast<cir::BaseClassAddrOp>(op)) {
                from = base.getDerivedAddr();
                to = base.getResult();
            }
            // The argument's own memory read as another type, to pass an
            // object in registers (a lambda that captures `this`), to a
            // function of this module that the files do not name, or to
            // return it from one: the bits stay in this module's code.
            auto coerced = [&](Operation *u) {
                if (auto st = dyn_cast<cir::StoreOp>(u)) return st.getAddr() == to && st.getValue() != to;
                auto ld = dyn_cast<cir::LoadOp>(u);
                if (!ld) return false;
                return llvm::all_of(ld->getUses(), [&](OpOperand &lu) {
                    if (auto r = dyn_cast<cir::ReturnOp>(lu.getOwner())) {
                        auto f = r->getParentOfType<cir::FuncOp>();
                        return f && !namedThere(f);
                    }
                    auto call = dyn_cast<cir::CallOp>(lu.getOwner());
                    cir::FuncOp g = call && !call.isIndirect() ? lookupCallee(call) : cir::FuncOp();
                    return g && !g.getBody().empty() && !namedThere(g);
                });
            };
            // A global's destructor registered with __cxa_atexit, which
            // hands the object back to it, a function of this module.
            auto atExit = [&](OpOperand &use) {
                auto call = dyn_cast<cir::CallOp>(use.getOwner());
                cir::FuncOp g = call ? lookupCallee(call) : cir::FuncOp();
                if (!g || g.getSymName() != "__cxa_atexit" || use.getOperandNumber() != 1 ||
                    call.getArgOperands().size() < 2)
                    return false;
                Value fn = call.getArgOperands()[0];
                while (auto cv = fn.getDefiningOp<cir::CastOp>()) fn = cv.getSrc();
                auto gg = fn.getDefiningOp<cir::GetGlobalOp>();
                auto dtor = gg ? module.lookupSymbol<cir::FuncOp>(gg.getName()) : cir::FuncOp();
                return dtor && !dtor.getBody().empty() && !namedThere(dtor);
            };
            if (from && reaches(from.getType()) && !reaches(to.getType()) &&
                !llvm::all_of(to.getUses(), [&](OpOperand &use) {
                    auto call = dyn_cast<cir::CallOp>(use.getOwner());
                    return (call && deallocates(lookupCallee(call))) || coerced(use.getOwner()) || atExit(use);
                })) {
                bad = where(op) + " converts a pointer to " + cls + ", or to what holds or points to it, to " +
                      typeName(to.getType()) + ", which the pass does not follow";
                return WalkResult::interrupt();
            }
            return WalkResult::advance();
        });
    if (!bad.empty()) v.refuse("(f) " + bad);
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
                if (isa<cir::PtrStrideOp, cir::CmpOp>(user)) continue;
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
    // Nothing another unit sees: the struct's functions, a function that
    // takes or returns what holds or points to the queue, and a global
    // whose type holds or points to it, at any depth.
    for (Operation *f : ops)
        if (names(cast<cir::FuncOp>(f).getSymName()))
            return no("(a) " + cast<cir::FuncOp>(f).getSymName() + " is named by another file linked with this one");
    // A search of the types a type is made of, through pointers: each is
    // looked at once, so a list that points to itself ends.
    auto reaches = [&](Type root) {
        llvm::SmallVector<Type, 16> work{root};
        llvm::DenseSet<Type> seen{root};
        while (!work.empty()) {
            Type t = work.pop_back_val();
            if (t == Type(rec)) return true;
            llvm::SmallVector<Type, 8> next;
            if (auto p = dyn_cast<cir::PointerType>(t)) next.push_back(p.getPointee());
            else if (auto a = dyn_cast<cir::ArrayType>(t)) next.push_back(a.getElementType());
            else if (auto r = dyn_cast<cir::RecordType>(t); r && !r.isIncomplete())
                next.append(r.getMembers().begin(), r.getMembers().end());
            for (Type n : next)
                if (seen.insert(n).second) work.push_back(n);
        }
        return false;
    };
    // A function of this module that another file calls: what it is
    // handed and what it hands back. (A call of a function with no body
    // here is checked above, by what it is passed.)
    for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
        if (f.isDeclaration() || !names(f.getSymName())) continue;
        auto ft = f.getFunctionType();
        if (llvm::any_of(ft.getInputs(), reaches) || llvm::any_of(ft.getReturnTypes(), reaches))
            return no("(a) " + f.getSymName() + ", named by another file linked with this one, takes or returns a pointer that reaches the queue");
    }
    for (auto g : module.getOps<cir::GlobalOp>())
        if (reaches(g.getSymType()) && names(g.getSymName()))
            return no("(a) " + g.getSymName() + " is named by another file linked with this one");
    return true;
}
