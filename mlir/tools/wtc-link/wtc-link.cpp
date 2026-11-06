// wtc-link: several CIR modules, one per translation unit, linked into one,
// as the system linker links their objects, so that the analyses see the
// whole program (docs/lifting.md, "Several files").
//
//   wtc-link a.cir b.cir ... -o all.cir
//
// What the link keeps is what each unit means compiled apart:
//
//   - a symbol of internal linkage (a static function or variable, a
//     string literal, a function-local static of C, what an anonymous
//     namespace holds) is the unit's own: where its name is taken by a
//     symbol of another unit, or by another unit's own, it is renamed,
//     name.uN for unit N, in every use in its unit. A header's static, or
//     two files' `static int x;`, stays one object per file;
//   - a symbol of external linkage is one symbol for the program: its one
//     strong definition is kept, or else the first of its weak, common or
//     inline (linkonce) definitions, as the linker keeps one copy of an
//     inline function; declarations go. Two strong definitions are an
//     error, as they are for the linker;
//   - a record type is the program's when every unit that defines its name
//     defines it the same; one that a unit defines otherwise (C's struct
//     tags are the unit's own) is renamed for that unit, as are the
//     records that hold it;
//   - the module's record layouts are merged, and its constructors and
//     destructors (cir.global_ctors, cir.global_dtors) are concatenated in
//     the order of the units.

#include "WTC/WTCDialect.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/AsmState.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/IR/Verifier.h"
#include "mlir/InitAllDialects.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Support/FileUtilities.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace mlir;

namespace {

llvm::cl::list<std::string> inputs(llvm::cl::Positional, llvm::cl::OneOrMore, llvm::cl::desc("<input.cir>..."));
llvm::cl::opt<std::string> output("o", llvm::cl::desc("Output file"), llvm::cl::init("-"));

void registerDialects(DialectRegistry &registry) {
    registerAllDialects(registry);
    registry.insert<cir::CIRDialect, mlir::wtc::WTCDialect>();
}

int fail(const llvm::Twine &why) {
    llvm::errs() << "wtc-link: " << why << "\n";
    return 1;
}

// The front end names a record without a name of its own "anon.N", N
// counted per unit: the same anonymous struct of a header is anon.0 in one
// unit and anon.1 in another.
bool isAnonymous(StringRef name) { return name.starts_with("anon."); }

// A record's structure: its kind and members, a named member record by its
// name (its own structure is compared on its own), an anonymous one by its
// structure, which is all that identifies it.
std::string structureOf(cir::RecordType r, unsigned depth = 0) {
    std::string s;
    llvm::raw_string_ostream os(s);
    os << (r.getPacked() ? "packed " : "") << (r.getPadded() ? "padded " : "") << (r.isUnion() ? "union" : "struct")
       << (r.isIncomplete() ? " incomplete" : "") << " {";
    for (Type mt : r.getMembers()) {
        auto mr = dyn_cast<cir::RecordType>(mt);
        if (mr && mr.getName() && !isAnonymous(mr.getName().getValue()))
            os << "\"" << mr.getName().getValue() << "\"";
        else if (mr && depth < 32)
            os << structureOf(mr, depth + 1);
        else
            os << mt;
        os << ", ";
    }
    os << "}";
    return s;
}

// The complete records a module defines, by name, with their structure.
std::map<std::string, std::string> recordsOf(ModuleOp m) {
    std::map<std::string, std::string> out;
    auto note = [&](Type t) {
        t.walk([&](cir::RecordType r) {
            if (!r.getName() || r.isIncomplete()) return;
            out.emplace(r.getName().getValue().str(), structureOf(r));
        });
    };
    m.walk([&](Operation *op) {
        for (Type t : op->getResultTypes()) note(t);
        for (Value v : op->getOperands()) note(v.getType());
        for (Region &r : op->getRegions())
            for (Block &b : r)
                for (BlockArgument a : b.getArguments()) note(a.getType());
        for (NamedAttribute a : op->getAttrs()) {
            a.getValue().walk([&](TypeAttr ta) { note(ta.getValue()); });
            if (auto ta = dyn_cast<TypeAttr>(a.getValue())) note(ta.getValue());
        }
    });
    return out;
}

// The text of a module with its records renamed, all at once (a rename may
// swap two names): a record's name appears in the printed form only as the
// quoted string after `!cir.struct<` or `!cir.union<` and the keywords that
// may follow them.
std::string renameRecords(StringRef text, const std::map<std::string, std::string> &rename) {
    std::string out;
    out.reserve(text.size());
    size_t at = 0;
    while (true) {
        size_t s1 = text.find("!cir.struct<", at), s2 = text.find("!cir.union<", at);
        size_t s = std::min(s1, s2);
        if (s == StringRef::npos) break;
        size_t q = s + (s == s1 ? strlen("!cir.struct<") : strlen("!cir.union<"));
        // Keywords before the name: class, struct, union, packed.
        while (q < text.size() && llvm::isAlpha(text[q])) {
            size_t e = q;
            while (e < text.size() && llvm::isAlpha(text[e])) ++e;
            if (e < text.size() && text[e] == ' ') q = e + 1;
            else break;
        }
        if (q >= text.size() || text[q] != '"') {
            out += text.substr(at, q - at).str();
            at = q;
            continue;
        }
        size_t end = text.find('"', q + 1);
        if (end == StringRef::npos) break;
        out += text.substr(at, q + 1 - at).str();
        std::string name = text.substr(q + 1, end - q - 1).str();
        auto it = rename.find(name);
        out += it == rename.end() ? name : it->second;
        at = end;
    }
    out += text.substr(at).str();
    return out;
}

bool isLocal(Operation *op) {
    if (auto f = dyn_cast<cir::FuncOp>(op)) return cir::isLocalLinkage(f.getLinkage());
    if (auto g = dyn_cast<cir::GlobalOp>(op)) return cir::isLocalLinkage(g.getLinkage());
    return false;
}
bool defines(Operation *op) {
    if (auto f = dyn_cast<cir::FuncOp>(op)) return !f.getBody().empty() || f.getAliasee();
    if (auto g = dyn_cast<cir::GlobalOp>(op)) return !g.isDeclaration();
    return true;
}
// A definition another unit's may replace: weak, common, inline.
// How strongly a symbol defines its name, as the linker ranks them: a
// strong definition, then a weak, common or inline one, then one only
// available externally (an `extern template` instantiation, which is not
// emitted and stands for the one another file defines), then a
// declaration.
enum Rank { Declaration, Available, Replaceable, Strong };
cir::GlobalLinkageKind linkageOf(Operation *op) {
    if (auto f = dyn_cast<cir::FuncOp>(op)) return f.getLinkage();
    if (auto g = dyn_cast<cir::GlobalOp>(op)) return g.getLinkage();
    return cir::GlobalLinkageKind::ExternalLinkage;
}
Rank rankOf(Operation *op) {
    if (!defines(op)) return Declaration;
    const cir::GlobalLinkageKind k = linkageOf(op);
    if (k == cir::GlobalLinkageKind::AvailableExternallyLinkage) return Available;
    if (cir::isWeakForLinker(k)) return Replaceable;
    return Strong;
}

// The size a global's type takes, for common symbols of several sizes,
// of which the linker keeps the largest.
uint64_t sizeOf(cir::GlobalOp g) {
    Type t = g.getSymType();
    uint64_t n = 1;
    while (auto a = dyn_cast<cir::ArrayType>(t)) {
        n *= a.getSize();
        t = a.getElementType();
    }
    if (auto it = dyn_cast<cir::IntType>(t)) return n * it.getWidth();
    return n;
}

}  // namespace

int main(int argc, char **argv) {
    llvm::InitLLVM init(argc, argv);
    llvm::cl::ParseCommandLineOptions(argc, argv, "Link CIR modules into one\n");

    // Each unit alone first: which record names the units define
    // differently, and so must keep apart.
    std::vector<std::string> texts;
    for (const std::string &in : inputs) {
        auto buf = llvm::MemoryBuffer::getFileOrSTDIN(in);
        if (!buf) return fail("cannot read " + in);
        texts.push_back((*buf)->getBuffer().str());
    }
    std::vector<std::map<std::string, std::string>> renames(inputs.size());
    {
        std::map<std::string, std::string> first;       // name -> structure, as the first unit to define it has it
        std::map<std::string, std::string> anonByShape;  // structure -> the name an anonymous record has in the link
        std::set<std::string> used;                      // record names taken in the link
        for (size_t i = 0; i < inputs.size(); ++i) {
            DialectRegistry registry;
            registerDialects(registry);
            MLIRContext ctx(registry);
            ctx.loadAllAvailableDialects();
            ParserConfig config(&ctx);
            OwningOpRef<ModuleOp> m = parseSourceString<ModuleOp>(texts[i], config, inputs[i]);
            if (!m) return fail("cannot parse " + inputs[i]);
            std::map<std::string, std::string> recs = recordsOf(*m);
            // A named record another unit defines otherwise is this unit's
            // own, and so is a record that holds one.
            std::set<std::string> apart;
            for (auto &[name, shape] : recs) {
                if (isAnonymous(name)) continue;
                auto [it, fresh] = first.emplace(name, shape);
                if (!fresh && it->second != shape) apart.insert(name);
            }
            for (bool grew = true; grew;) {
                grew = false;
                for (auto &[name, shape] : recs) {
                    if (isAnonymous(name) || apart.count(name)) continue;
                    for (const std::string &x : apart)
                        if (shape.find("\"" + x + "\"") != std::string::npos) {
                            apart.insert(name);
                            grew = true;
                            break;
                        }
                }
            }
            // An anonymous record takes the name of the same structure in
            // an earlier unit, or a name of its own.
            for (auto &[name, shape] : recs) {
                if (!isAnonymous(name)) continue;
                const bool holdsApart = llvm::any_of(
                    apart, [&](const std::string &x) { return shape.find("\"" + x + "\"") != std::string::npos; });
                if (holdsApart) {
                    std::string own = name + ".u" + std::to_string(i);
                    renames[i][name] = own;
                    used.insert(own);
                    continue;
                }
                auto [it, fresh] = anonByShape.emplace(shape, name);
                if (fresh && used.count(name)) {
                    std::string own = name + ".u" + std::to_string(i);
                    it->second = own;
                }
                if (it->second != name) renames[i][name] = it->second;
                used.insert(it->second);
            }
            for (const std::string &x : apart) renames[i][x] = x + ".u" + std::to_string(i);
            for (auto &[name, shape] : recs)
                if (!isAnonymous(name)) used.insert(renames[i].count(name) ? renames[i][name] : name);
            if (!renames[i].empty()) texts[i] = renameRecords(texts[i], renames[i]);
        }
    }

    DialectRegistry registry;
    registerDialects(registry);
    MLIRContext ctx(registry);
    ctx.loadAllAvailableDialects();
    ParserConfig config(&ctx);
    std::vector<OwningOpRef<ModuleOp>> units;
    for (size_t i = 0; i < inputs.size(); ++i) {
        units.push_back(parseSourceString<ModuleOp>(texts[i], config, inputs[i]));
        if (!units.back()) return fail("cannot parse " + inputs[i] + " with the units before it");
    }

    // Every name any unit uses, so that a renamed symbol takes a free one.
    llvm::StringSet<> taken;
    for (auto &m : units)
        for (Operation &op : *m->getBody())
            if (auto s = SymbolTable::getSymbolName(&op)) taken.insert(s.getValue());

    // The units' own symbols: the first of a name keeps it, unless an
    // external symbol of any unit has it; the others are renamed.
    llvm::StringSet<> external, kept;
    for (auto &m : units)
        for (Operation &op : *m->getBody())
            if (auto s = SymbolTable::getSymbolName(&op); s && !isLocal(&op)) external.insert(s.getValue());
    // A name a unit uses and does not define (an alias of a static the
    // front end did not emit) is external too: it is never bound to
    // another unit's own symbol.
    for (auto &m : units)
        m->walk([&](Operation *op) {
            for (NamedAttribute a : op->getAttrs())
                a.getValue().walk([&](SymbolRefAttr r) {
                    StringRef n = r.getRootReference().getValue();
                    if (!SymbolTable::lookupSymbolIn(*m, r.getRootReference())) external.insert(n);
                });
        });
    for (size_t i = 0; i < units.size(); ++i) {
        ModuleOp m = *units[i];
        llvm::SmallVector<Operation *, 16> own;
        for (Operation &op : *m.getBody())
            if (SymbolTable::getSymbolName(&op) && isLocal(&op)) own.push_back(&op);
        for (Operation *op : own) {
            StringRef name = SymbolTable::getSymbolName(op).getValue();
            if (!external.contains(name) && kept.insert(name).second) continue;
            std::string fresh = (name + ".u" + llvm::Twine(i)).str();
            for (unsigned k = 2; taken.contains(fresh); ++k) fresh = (name + ".u" + llvm::Twine(i) + "." + llvm::Twine(k)).str();
            taken.insert(fresh);
            kept.insert(fresh);
            auto newName = StringAttr::get(&ctx, fresh);
            if (failed(SymbolTable::replaceAllSymbolUses(op, newName, m))) return fail("cannot rename " + name);
            // The module's own attributes name its constructors and
            // destructors by their names.
            for (const char *list : {"cir.global_ctors", "cir.global_dtors"})
                if (auto a = m->getAttrOfType<ArrayAttr>(list)) {
                    llvm::SmallVector<Attribute, 4> out;
                    for (Attribute e : a) {
                        if (auto c = dyn_cast<cir::GlobalCtorAttr>(e); c && c.getName().getValue() == name)
                            e = cir::GlobalCtorAttr::get(newName, c.getPriority());
                        else if (auto d = dyn_cast<cir::GlobalDtorAttr>(e); d && d.getName().getValue() == name)
                            e = cir::GlobalDtorAttr::get(newName, d.getPriority());
                        out.push_back(e);
                    }
                    m->setAttr(list, ArrayAttr::get(&ctx, out));
                }
            SymbolTable::setSymbolName(op, newName);
        }
    }

    // One symbol for each external name: its strong definition, else its
    // first weak, common or inline one (the largest of common ones), else
    // its first available-externally one, else its first declaration.
    llvm::StringMap<Operation *> chosen;
    for (auto &m : units)
        for (Operation &op : *m->getBody()) {
            auto s = SymbolTable::getSymbolName(&op);
            if (!s || isLocal(&op)) continue;
            auto [it, fresh] = chosen.try_emplace(s.getValue(), &op);
            if (fresh) continue;
            Operation *have = it->second;
            const Rank h = rankOf(have), r = rankOf(&op);
            if (h == Strong && r == Strong) return fail("multiple definition of " + s.getValue());
            bool take = r > h;
            if (r == h && linkageOf(&op) == cir::GlobalLinkageKind::CommonLinkage) {
                auto a = dyn_cast<cir::GlobalOp>(have), b = dyn_cast<cir::GlobalOp>(&op);
                take = a && b && sizeOf(b) > sizeOf(a);
            }
            if (take) it->second = &op;
        }

    // A unit's dynamic initialiser of a global defined inline in every unit
    // that uses it (an inline variable, a template's static member) runs
    // only in the unit whose definition is kept: the front end guards it
    // with no guard variable, and each unit's would run once more.
    for (auto &m : units) {
        llvm::SmallPtrSet<Operation *, 4> dropped;
        for (cir::FuncOp f : m->getOps<cir::FuncOp>()) {
            if (!f.getSymName().starts_with("__cxx_global_var_init") || f.getBody().empty()) continue;
            cir::GetGlobalOp first;
            f.walk([&](cir::GetGlobalOp g) {
                if (!first && SymbolTable::lookupSymbolIn(*m, g.getNameAttr()) &&
                    isa<cir::GlobalOp>(SymbolTable::lookupSymbolIn(*m, g.getNameAttr())))
                    first = g;
            });
            if (!first) continue;
            Operation *target = SymbolTable::lookupSymbolIn(*m, first.getNameAttr());
            if (isLocal(target) || rankOf(target) != Replaceable) continue;
            if (chosen.lookup(first.getName()) != target) dropped.insert(f);
        }
        if (dropped.empty()) continue;
        llvm::SmallVector<Operation *, 4> calls;
        m->walk([&](cir::CallOp c) {
            if (auto callee = c.getCalleeAttr())
                if (Operation *g = SymbolTable::lookupSymbolIn(*m, callee); g && dropped.contains(g)) calls.push_back(c);
        });
        for (Operation *c : calls) c->erase();
        for (const char *list : {"cir.global_ctors"})
            if (auto a = (*m)->getAttrOfType<ArrayAttr>(list)) {
                llvm::SmallVector<Attribute, 4> out;
                for (Attribute e : a) {
                    auto c = dyn_cast<cir::GlobalCtorAttr>(e);
                    Operation *g = c ? SymbolTable::lookupSymbolIn(*m, c.getName()) : nullptr;
                    if (!g || !dropped.contains(g)) out.push_back(e);
                }
                (*m)->setAttr(list, ArrayAttr::get(&ctx, out));
            }
        for (Operation *f : dropped) f->erase();
    }

    // The merge, in the order of the units; the attributes merged.
    ModuleOp into = *units[0];
    llvm::SmallVector<NamedAttribute, 8> layouts;
    llvm::SmallVector<Attribute, 8> ctors, dtors;
    llvm::SmallVector<Attribute, 4> asmLines;
    for (size_t i = 0; i < units.size(); ++i) {
        ModuleOp m = *units[i];
        if (auto d = m->getAttrOfType<DictionaryAttr>("cir.record_layouts"))
            for (NamedAttribute a : d) {
                std::string key = a.getName().str();
                if (auto it = renames[i].find(key); it != renames[i].end()) key = it->second;
                if (llvm::none_of(layouts, [&](NamedAttribute x) { return x.getName() == key; }))
                    layouts.push_back(NamedAttribute(StringAttr::get(&ctx, key), a.getValue()));
            }
        if (auto a = m->getAttrOfType<ArrayAttr>("cir.global_ctors")) ctors.append(a.begin(), a.end());
        if (auto a = m->getAttrOfType<ArrayAttr>("cir.global_dtors")) dtors.append(a.begin(), a.end());
        if (auto a = m->getAttrOfType<ArrayAttr>("cir.module_asm")) asmLines.append(a.begin(), a.end());
        if (i == 0) {
            for (Operation &op : llvm::make_early_inc_range(*m.getBody())) {
                auto s = SymbolTable::getSymbolName(&op);
                if (s && !isLocal(&op) && chosen.lookup(s.getValue()) != &op) op.erase();
            }
            continue;
        }
        for (Operation &op : llvm::make_early_inc_range(*m.getBody())) {
            auto s = SymbolTable::getSymbolName(&op);
            if (s && !isLocal(&op) && chosen.lookup(s.getValue()) != &op) {
                op.erase();
                continue;
            }
            op.moveBefore(into.getBody(), into.getBody()->end());
        }
    }
    if (!layouts.empty()) into->setAttr("cir.record_layouts", DictionaryAttr::get(&ctx, layouts));
    if (!ctors.empty()) into->setAttr("cir.global_ctors", ArrayAttr::get(&ctx, ctors));
    if (!dtors.empty()) into->setAttr("cir.global_dtors", ArrayAttr::get(&ctx, dtors));
    into->setAttr("cir.module_asm", ArrayAttr::get(&ctx, asmLines));
    // An alias whose aliasee its file did not emit (the front end drops an
    // unused static an alias names) names nothing the link can bind: no
    // other file's symbol of that name is the one meant.
    for (cir::FuncOp f : into.getOps<cir::FuncOp>())
        if (auto a = f.getAliaseeAttr(); a && !SymbolTable::lookupSymbolIn(into, a))
            return fail(f.getSymName() + " is an alias of " + a.getValue() +
                        ", which its file does not define; compile that file with the definition kept");
    // A unit that declared a global with another type than the one kept
    // (`extern int t[];` for `int t[4];`, a common symbol of another size)
    // reads it as its own type: the address is taken as the kept type and
    // cast to the one the unit's code uses.
    {
        llvm::SmallVector<cir::GetGlobalOp, 8> mismatched;
        into.walk([&](cir::GetGlobalOp g) {
            auto def = dyn_cast_or_null<cir::GlobalOp>(SymbolTable::lookupSymbolIn(into, g.getNameAttr()));
            auto pt = dyn_cast<cir::PointerType>(g.getType());
            if (def && pt && pt.getPointee() != def.getSymType()) mismatched.push_back(g);
        });
        for (cir::GetGlobalOp g : mismatched) {
            auto def = cast<cir::GlobalOp>(SymbolTable::lookupSymbolIn(into, g.getNameAttr()));
            Type want = g.getType();
            OpBuilder b(g->getContext());
            b.setInsertionPointAfter(g);
            g.getResult().setType(cir::PointerType::get(def.getSymType()));
            auto cast = cir::CastOp::create(b, g.getLoc(), want, cir::CastKind::bitcast, g.getResult());
            g.getResult().replaceAllUsesExcept(cast.getResult(), cast);
        }
    }
    // A chosen declaration was moved before the definition it names, or
    // erased in its favour: references resolve by name either way.
    if (failed(verify(into))) return fail("the linked module does not verify");

    std::string err;
    auto out = openOutputFile(output, &err);
    if (!out) return fail(err);
    OpPrintingFlags flags;
    flags.enableDebugInfo(true, false);
    into->print(out->os(), flags);
    out->os() << "\n";
    out->keep();
    return 0;
}
