#include "WTC/Transforms/LiftedBounded.h"
#include "WTC/Transforms/Utils.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"

#include "mlir/IR/SymbolTable.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"

#include <algorithm>
#include <functional>

using namespace mlir;
using namespace mlir::wtc;

namespace {

struct Name {
    std::string base, context;
    bool ctor = false, dtor = false, ok = false;
};

Name nameOf(StringRef mangled) {
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
    n.ctor = d.isCtorOrDtor() && !StringRef(n.base).starts_with("~");
    n.dtor = d.isCtorOrDtor() && StringRef(n.base).starts_with("~");
    n.ok = true;
    return n;
}

StringRef recordName(Type t) {
    auto r = dyn_cast_or_null<cir::RecordType>(t);
    return r && r.getName() ? r.getName().getValue() : StringRef();
}

bool isStdForward(const Name &n) { return n.context == "std" && (n.base == "move" || n.base == "forward"); }

// ---------------------------------------------------------------------------
// Symbolic values, as canonical strings: "@i" the value member i had at
// entry, "P<k>" parameter k, "*P<k>" what parameter k points to, "#<n>" a
// constant, "#true"/"#false", and ops over them. Commutative operands are
// sorted and a > b is written b < a, so that `t + 1 == h` and `h == 1 + t`
// are one string.

using Sym = std::string;

Sym k(int64_t v) { return "#" + std::to_string(v); }
Sym op2(StringRef f, Sym a, Sym b, bool commutative) {
    if (commutative && b < a) std::swap(a, b);
    return (f + "(" + a + "," + b + ")").str();
}
Sym add(const Sym &a, const Sym &b) { return op2("add", a, b, true); }
Sym sub(const Sym &a, const Sym &b) { return op2("sub", a, b, false); }
Sym rem(const Sym &a, const Sym &b) { return op2("rem", a, b, false); }
Sym eq(const Sym &a, const Sym &b) { return op2("eq", a, b, true); }
Sym ne(const Sym &a, const Sym &b) { return op2("ne", a, b, true); }
Sym lt(const Sym &a, const Sym &b) { return op2("lt", a, b, false); }
Sym le(const Sym &a, const Sym &b) { return op2("le", a, b, false); }
Sym ite(const Sym &c, const Sym &a, const Sym &b) {
    if (a == b || c == "#true") return a;
    if (c == "#false") return b;
    return "ite(" + c + "," + a + "," + b + ")";
}
Sym notS(const Sym &a) {
    if (a == "#true") return "#false";
    if (a == "#false") return "#true";
    StringRef s(a);
    if (s.starts_with("eq(")) return "ne(" + s.drop_front(3).str();
    if (s.starts_with("ne(")) return "eq(" + s.drop_front(3).str();
    if (s.starts_with("not(") && s.ends_with(")")) return s.drop_front(4).drop_back(1).str();
    return "not(" + a + ")";
}
Sym mem(unsigned i) { return "@" + std::to_string(i); }

} // namespace

// What a function computes: its result, the members it leaves changed, and
// what it does to the vector's elements, in order.
struct RingModel::Effect {
    Sym ret;  // empty for void
    std::map<unsigned, Sym> members;
    std::vector<std::string> effects;

    std::string str() const {
        std::string s = "ret=" + ret + " mem{";
        for (auto &[i, v] : members) s += mem(i) + ":" + v + ";";
        s += "} eff[";
        for (auto &e : effects) s += e + ";";
        return s + "]";
    }
};

namespace {

// One path through a function: what its SSA values, members and stack
// slots hold.
struct Path {
    llvm::DenseMap<Value, Sym> vals;
    std::map<unsigned, Sym> members;
    std::map<Operation *, Sym> slots;
    std::vector<std::string> effects;
};

// Where to go on at the end of a nested region.
struct Frame {
    Block::iterator it;
    Block *block;
    const Frame *up;
};

class Evaluator {
public:
    Evaluator(cir::FuncOp f, cir::RecordType rec, std::optional<unsigned> slots) : f(f), rec(rec), slots(slots) {}

    std::string why;

    // The function's effect, merged over its paths, or none.
    std::optional<std::tuple<Sym, std::map<unsigned, Sym>, std::vector<std::string>>> run() {
        Block &entry = f.getBody().front();
        Path p;
        for (BlockArgument a : entry.getArguments())
            p.vals[a] = a.getArgNumber() == 0 ? Sym("this") : "P" + std::to_string(a.getArgNumber());
        return go(entry.begin(), &entry, std::move(p), nullptr, 0);
    }

private:
    using Out = std::optional<std::tuple<Sym, std::map<unsigned, Sym>, std::vector<std::string>>>;
    cir::FuncOp f;
    cir::RecordType rec;
    std::optional<unsigned> slots;  // the slot-count member, once known: its value is at least one
    std::map<Operation *, unsigned> slotIds;

    Out fail(const Twine &w) {
        if (why.empty()) why = w.str();
        return std::nullopt;
    }

    std::optional<Sym> val(Path &p, Value v) {
        auto it = p.vals.find(v);
        if (it != p.vals.end()) return it->second;
        if (auto a = v.getDefiningOp<cir::AllocaOp>()) {
            auto [s, fresh] = slotIds.try_emplace(a, slotIds.size());
            return "slot:" + std::to_string(s->second);
        }
        why = why.empty() ? "reads a value the pass cannot follow" : why;
        return std::nullopt;
    }

    // The guard `slots > 0` (or `!= 0`) is true: the constructor makes the
    // slot count the capacity plus one.
    Sym simplifyGuard(const Sym &c) {
        if (!slots) return c;
        const Sym m = mem(*slots);
        if (c == lt(k(0), m) || c == ne(k(0), m)) return "#true";
        if (c == eq(k(0), m) || c == le(m, k(0))) return "#false";
        return c;
    }

    Out merge(const Sym &c, Out a, Out b) {
        if (!a || !b) return std::nullopt;
        auto &[ra, ma, ea] = *a;
        auto &[rb, mb, eb] = *b;
        if (ea != eb) return fail("does something to the elements on one branch only");
        std::map<unsigned, Sym> m;
        for (auto &[i, v] : ma) m[i] = ite(c, v, mb.count(i) ? mb[i] : mem(i));
        for (auto &[i, v] : mb)
            if (!ma.count(i)) m[i] = ite(c, mem(i), v);
        for (auto it = m.begin(); it != m.end();)
            it = it->second == mem(it->first) ? m.erase(it) : std::next(it);
        return std::make_tuple(ite(c, ra, rb), m, ea);
    }

    Out go(Block::iterator it, Block *b, Path p, const Frame *up, unsigned depth) {
        if (depth > 64) return fail("is too deep to read");
        for (; it != b->end(); ++it) {
            Operation *op = &*it;
            if (isa<cir::AllocaOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(op)) continue;
            if (auto c = dyn_cast<cir::ConstantOp>(op)) {
                if (auto i = dyn_cast<cir::IntAttr>(c.getValue())) p.vals[c] = k(i.getValue().getSExtValue());
                else if (auto bo = dyn_cast<cir::BoolAttr>(c.getValue())) p.vals[c] = bo.getValue() ? "#true" : "#false";
                else return fail("uses a constant the pass cannot read");
                continue;
            }
            if (auto gm = dyn_cast<cir::GetMemberOp>(op)) {
                auto base = val(p, gm.getAddr());
                if (!base) return std::nullopt;
                if (*base != "this") return fail("reaches a member of something other than its object");
                p.vals[gm] = "mem:" + std::to_string(gm.getIndex());
                continue;
            }
            if (auto ld = dyn_cast<cir::LoadOp>(op)) {
                auto a = val(p, ld.getAddr());
                if (!a) return std::nullopt;
                StringRef as(*a);
                if (as.starts_with("mem:")) {
                    unsigned i = 0;
                    as.drop_front(4).getAsInteger(10, i);
                    p.vals[ld] = p.members.count(i) ? p.members[i] : mem(i);
                } else if (as.starts_with("slot:")) {
                    Operation *slot = ld.getAddr().getDefiningOp();
                    auto s = slot ? p.slots.find(slot) : p.slots.end();
                    if (s == p.slots.end()) return fail("reads a local before writing it");
                    p.vals[ld] = s->second;
                } else if (as.starts_with("P")) {
                    p.vals[ld] = "*" + *a;
                } else {
                    return fail("reads memory other than its members and locals");
                }
                continue;
            }
            if (auto st = dyn_cast<cir::StoreOp>(op)) {
                auto v = val(p, st.getValue());
                auto a = val(p, st.getAddr());
                if (!v || !a) return std::nullopt;
                StringRef as(*a);
                if (as.starts_with("mem:")) {
                    unsigned i = 0;
                    as.drop_front(4).getAsInteger(10, i);
                    p.members[i] = *v;
                } else if (as.starts_with("slot:")) {
                    p.slots[st.getAddr().getDefiningOp()] = *v;
                } else if (as.starts_with("elem(")) {
                    p.effects.push_back("assign(" + *a + "," + *v + ")");
                } else {
                    return fail("writes memory other than its members, its locals and its elements");
                }
                continue;
            }
            if (isa<cir::AddOp, cir::SubOp, cir::RemOp, cir::CmpOp>(op)) {
                auto l = val(p, op->getOperand(0)), r = val(p, op->getOperand(1));
                if (!l || !r) return std::nullopt;
                Sym s;
                if (isa<cir::AddOp>(op)) s = add(*l, *r);
                else if (isa<cir::SubOp>(op)) s = sub(*l, *r);
                else if (isa<cir::RemOp>(op)) s = rem(*l, *r);
                else switch (cast<cir::CmpOp>(op).getKind()) {
                    case cir::CmpOpKind::eq: s = eq(*l, *r); break;
                    case cir::CmpOpKind::ne: s = ne(*l, *r); break;
                    case cir::CmpOpKind::lt: s = lt(*l, *r); break;
                    case cir::CmpOpKind::le: s = le(*l, *r); break;
                    case cir::CmpOpKind::gt: s = lt(*r, *l); break;
                    case cir::CmpOpKind::ge: s = le(*r, *l); break;
                    default: return fail("compares in a way the pass cannot read");
                    }
                p.vals[op->getResult(0)] = s;
                continue;
            }
            if (isa<cir::IncOp, cir::DecOp, cir::NotOp>(op)) {
                auto x = val(p, op->getOperand(0));
                if (!x) return std::nullopt;
                p.vals[op->getResult(0)] = isa<cir::IncOp>(op) ? add(*x, k(1))
                                           : isa<cir::DecOp>(op) ? sub(*x, k(1))
                                                                 : notS(*x);
                continue;
            }
            if (auto c = dyn_cast<cir::CastOp>(op)) {
                if (c.getKind() != cir::CastKind::integral && c.getKind() != cir::CastKind::bool_to_int)
                    return fail("converts a value in a way the pass cannot read");
                auto x = val(p, c.getSrc());
                if (!x) return std::nullopt;
                p.vals[c] = *x;
                continue;
            }
            if (auto t = dyn_cast<cir::TernaryOp>(op)) {
                auto c = val(p, t.getCond());
                if (!c) return std::nullopt;
                auto a = yielded(t.getTrueRegion(), p), e = yielded(t.getFalseRegion(), p);
                if (!a || !e) return fail("has a conditional expression the pass cannot read");
                p.vals[t.getResult()] = ite(*c, *a, *e);
                continue;
            }
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                if (!this->call(p, call)) return std::nullopt;
                continue;
            }
            if (auto sc = dyn_cast<cir::ScopeOp>(op)) {
                if (sc.getScopeRegion().empty()) continue;
                Frame fr{std::next(it), b, up};
                Block &inner = sc.getScopeRegion().front();
                return go(inner.begin(), &inner, std::move(p), &fr, depth + 1);
            }
            if (auto cl = dyn_cast<cir::CleanupScopeOp>(op)) {
                // The cleanup may only destroy locals (a temporary allocator).
                Region &cr = cl.getCleanupRegion();
                if (!cr.empty())
                    for (Operation &o : cr.front()) {
                        if (isa<cir::YieldOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(o)) continue;
                        auto c = dyn_cast<cir::CallOp>(o);
                        cir::FuncOp callee = c ? lookupCallee(c) : cir::FuncOp();
                        if (c && callee && nameOf(callee.getSymName()).dtor && c.getNumOperands() == 1 &&
                            c.getOperand(0).getDefiningOp<cir::AllocaOp>())
                            continue;
                        if (c && callee && nameOf(callee.getSymName()).dtor && c.getNumOperands() == 1 &&
                            cl.getCleanupKind() == cir::CleanupKind::EH)
                            continue;  // unwinding a member already built
                        return fail("has a cleanup other than destroying its temporaries");
                    }
                Frame fr{std::next(it), b, up};
                Block &inner = cl.getBodyRegion().front();
                return go(inner.begin(), &inner, std::move(p), &fr, depth + 1);
            }
            if (auto i = dyn_cast<cir::IfOp>(op)) {
                auto c0 = val(p, i.getCondition());
                if (!c0) return std::nullopt;
                const Sym c = simplifyGuard(*c0);
                Frame fr{std::next(it), b, up};
                auto branch = [&](Region &r, Path q) -> Out {
                    if (r.empty()) return go(fr.it, fr.block, std::move(q), fr.up, depth + 1);
                    Block &inner = r.front();
                    return go(inner.begin(), &inner, std::move(q), &fr, depth + 1);
                };
                if (c == "#true") return branch(i.getThenRegion(), std::move(p));
                if (c == "#false") return branch(i.getElseRegion(), std::move(p));
                Out a = branch(i.getThenRegion(), p);
                Out e = branch(i.getElseRegion(), std::move(p));
                return merge(c, std::move(a), std::move(e));
            }
            if (isa<cir::YieldOp>(op)) {
                if (!up) return fail("ends a region the pass did not enter");
                return go(up->it, up->block, std::move(p), up->up, depth + 1);
            }
            if (auto r = dyn_cast<cir::ReturnOp>(op)) {
                Sym ret;
                if (r.getNumOperands() == 1) {
                    auto v = val(p, r.getOperand(0));
                    if (!v) return std::nullopt;
                    ret = *v;
                }
                std::map<unsigned, Sym> m;
                for (auto &[i, v] : p.members)
                    if (v != mem(i)) m[i] = v;
                return std::make_tuple(ret, m, p.effects);
            }
            return fail("does something the pass cannot read (" + op->getName().getStringRef() + ")");
        }
        if (up) return go(up->it, up->block, std::move(p), up->up, depth + 1);
        return fail("falls off its end");
    }

    // The value a branch of a conditional expression yields: constants,
    // members and arithmetic on them, nothing with an effect.
    std::optional<Sym> yielded(Region &r, Path q) {
        if (r.empty()) return std::nullopt;
        for (Operation &o : r.front()) {
            if (auto y = dyn_cast<cir::YieldOp>(o)) {
                if (y.getNumOperands() != 1) return std::nullopt;
                return val(q, y.getOperand(0));
            }
            if (auto gm = dyn_cast<cir::GetMemberOp>(o)) {
                q.vals[gm] = "mem:" + std::to_string(gm.getIndex());
            } else if (auto cst = dyn_cast<cir::ConstantOp>(o)) {
                auto i = dyn_cast<cir::IntAttr>(cst.getValue());
                if (!i) return std::nullopt;
                q.vals[cst] = k(i.getValue().getSExtValue());
            } else if (auto ld = dyn_cast<cir::LoadOp>(o)) {
                auto a = val(q, ld.getAddr());
                unsigned i = 0;
                if (!a || !StringRef(*a).starts_with("mem:") || StringRef(*a).drop_front(4).getAsInteger(10, i))
                    return std::nullopt;
                q.vals[ld] = q.members.count(i) ? q.members[i] : mem(i);
            } else if (isa<cir::AddOp, cir::SubOp, cir::RemOp>(o)) {
                auto l = val(q, o.getOperand(0)), rr = val(q, o.getOperand(1));
                if (!l || !rr) return std::nullopt;
                q.vals[o.getResult(0)] = isa<cir::AddOp>(o) ? add(*l, *rr) : isa<cir::SubOp>(o) ? sub(*l, *rr) : rem(*l, *rr);
            } else if (auto c = dyn_cast<cir::CastOp>(o); c && c.getKind() == cir::CastKind::integral) {
                auto x = val(q, c.getSrc());
                if (!x) return std::nullopt;
                q.vals[c] = *x;
            } else {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool call(Path &p, cir::CallOp call) {
        cir::FuncOp callee = lookupCallee(call);
        if (!callee) return (void)fail("makes an indirect call"), false;
        const Name n = nameOf(callee.getSymName());
        llvm::SmallVector<Sym, 4> args;
        for (Value a : call.getOperands()) {
            auto s = val(p, a);
            if (!s) return false;
            args.push_back(*s);
        }
        if (isStdForward(n) && args.size() == 1) {
            p.vals[call.getResult()] = args[0];
            return true;
        }
        StringRef ctx(n.context);
        const bool onVector = !args.empty() && StringRef(args[0]).starts_with("mem:");
        auto vecMember = [&]() -> std::optional<unsigned> {
            unsigned i = 0;
            if (!onVector || StringRef(args[0]).drop_front(4).getAsInteger(10, i)) return std::nullopt;
            if (!recordName(rec.getElementType(i)).starts_with("std::vector<")) return std::nullopt;
            return i;
        };
        if (n.base == "operator[]" && ctx.starts_with("std::vector<") && args.size() == 2 && vecMember()) {
            p.vals[call.getResult()] = "elem(" + std::to_string(*vecMember()) + "," + args[1] + ")";
            return true;
        }
        if (n.base == "operator=" && args.size() == 2 && StringRef(args[0]).starts_with("elem(")) {
            p.effects.push_back("assign(" + args[0] + "," + args[1] + ")");
            if (call.getNumResults()) p.vals[call.getResult()] = args[0];
            return true;
        }
        if (n.ctor && ctx.starts_with("std::vector<") && vecMember() && args.size() >= 2) {
            p.effects.push_back("vinit(" + std::to_string(*vecMember()) + "," + args[1] + ")");
            return true;
        }
        // A temporary's own constructor and destructor (the allocator the
        // vector is sized with).
        if ((n.ctor || n.dtor) && !args.empty() && StringRef(args[0]).starts_with("slot:") &&
            llvm::all_of(llvm::ArrayRef(args).drop_front(),
                         [](const Sym &s) { return StringRef(s).starts_with("slot:"); })) {
            return true;
        }
        fail("calls " + llvm::demangle(callee.getSymName().str()));
        return false;
    }
};

} // namespace

const char *mlir::wtc::ringOpName(RingOp op) {
    switch (op) {
    case RingOp::Push: return "push (overrunning when full)";
    case RingOp::Front: return "front";
    case RingOp::PopFront: return "pop_front";
    case RingOp::Empty: return "empty";
    case RingOp::Full: return "full";
    case RingOp::Size: return "size";
    case RingOp::OverrunCounter: return "overrun count";
    case RingOp::ResetOverrunCounter: return "reset of the overrun count";
    case RingOp::None: break;
    }
    return "none";
}

std::optional<RingModel::Effect> RingModel::evaluate(cir::FuncOp f, std::string &reason) {
    if (f.getBody().empty()) {
        reason = "has no body here";
        return std::nullopt;
    }
    Evaluator e(f, rec_, ctor_ ? std::optional<unsigned>(slots_) : std::nullopt);
    auto r = e.run();
    if (!r) {
        reason = e.why.empty() ? "cannot be read" : e.why;
        return std::nullopt;
    }
    Effect out;
    std::tie(out.ret, out.members, out.effects) = *r;
    return out;
}

RingModel::RingModel(ModuleOp module, cir::RecordType rec) : module_(module), rec_(rec) {
    // Four integers and a std::vector, nothing else.
    llvm::SmallVector<unsigned, 4> ints;
    std::optional<unsigned> vec;
    llvm::ArrayRef<Type> members = rec.getMembers();
    for (unsigned i = 0; i < members.size(); ++i) {
        if (auto it = dyn_cast<cir::IntType>(members[i]); it && it.getWidth() == 64 && it.isUnsigned()) {
            ints.push_back(i);
        } else if (recordName(members[i]).starts_with("std::vector<") && !vec) {
            vec = i;
        } else {
            why_ = "it has a member that is neither a 64-bit unsigned count nor its one std::vector";
            return;
        }
    }
    if (ints.size() != 4 || !vec) {
        why_ = "it is not four counts and a std::vector";
        return;
    }
    vec_ = *vec;
    // The constructor that takes the capacity: slots = capacity + 1, the
    // other three zero, the vector sized by the slots.
    std::string last;
    for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
        if (!f.isCxxConstructor() || f.getBody().empty() || f.getNumArguments() != 2) continue;
        auto pt = dyn_cast<cir::PointerType>(f.getArgumentTypes()[0]);
        auto it = dyn_cast<cir::IntType>(f.getArgumentTypes()[1]);
        if (!pt || pt.getPointee() != rec || !it) continue;
        std::string reason;
        auto e = evaluate(f, reason);
        if (!e) {
            last = "its constructor " + reason;
            continue;
        }
        const Sym slotsVal = add("P1", k(1));
        std::optional<unsigned> s;
        llvm::SmallVector<unsigned, 3> zero;
        bool ok = e->ret.empty();
        for (unsigned i : ints) {
            auto m = e->members.find(i);
            if (m == e->members.end()) ok = false;
            else if (m->second == slotsVal && !s) s = i;
            else if (m->second == k(0)) zero.push_back(i);
            else ok = false;
        }
        if (!ok || !s || zero.size() != 3 || e->effects.size() != 1 ||
            e->effects.front() != "vinit(" + std::to_string(vec_) + "," + slotsVal + ")") {
            last = "its constructor does not set the slots to the capacity plus one, the rest to zero, and "
                   "size the vector by the slots (" +
                   e->str() + ")";
            continue;
        }
        ctor_ = f;
        slots_ = *s;
        std::array<unsigned, 3> z{zero[0], zero[1], zero[2]};
        std::sort(z.begin(), z.end());
        do roles_.push_back(z);
        while (std::next_permutation(z.begin(), z.end()));
        break;
    }
    if (!ctor_) why_ = last.empty() ? "it has no constructor here that takes its capacity" : last;
}

RingOp RingModel::classify(cir::FuncOp f, std::string &reason) {
    if (!why_.empty()) {
        reason = why_;
        return RingOp::None;
    }
    if (auto it = done_.find(f); it != done_.end()) return it->second;
    auto e = evaluate(f, reason);
    if (!e) return done_[f] = RingOp::None;
    const std::string got = e->str();

    // The forms, under an assignment of head, tail and count.
    auto expected = [&](RingOp op, unsigned h, unsigned t, unsigned o) {
        const Sym H = mem(h), T = mem(t), O = mem(o), M = mem(slots_);
        auto next = [&](const Sym &x) { return rem(add(x, k(1)), M); };
        const std::string V = std::to_string(vec_);
        Effect x;
        switch (op) {
        case RingOp::Empty: x.ret = eq(T, H); break;
        case RingOp::Full: x.ret = eq(next(T), H); break;
        case RingOp::Size: x.ret = ite(le(H, T), sub(T, H), sub(M, sub(H, T))); break;
        case RingOp::Front: x.ret = "elem(" + V + "," + H + ")"; break;
        case RingOp::PopFront: x.members[h] = next(H); break;
        case RingOp::OverrunCounter: x.ret = O; break;
        case RingOp::ResetOverrunCounter: x.members[o] = k(0); break;
        case RingOp::Push: {
            const Sym over = eq(next(T), H);
            x.members[t] = next(T);
            x.members[h] = ite(over, next(H), H);
            x.members[o] = ite(over, add(O, k(1)), O);
            x.effects.push_back("assign(elem(" + V + "," + T + "),P1)");
            break;
        }
        case RingOp::None: break;
        }
        return x.str();
    };
    // A scalar element is written by a store of what the parameter points
    // to, a class by its assignment from the parameter.
    std::string gotClass = got;
    if (auto pos = gotClass.find("),*P1)"); pos != std::string::npos) gotClass.replace(pos, 6, "),P1)");

    RingOp found = RingOp::None;
    llvm::SmallVector<std::array<unsigned, 3>, 6> keep;
    for (RingOp op : {RingOp::Push, RingOp::Front, RingOp::PopFront, RingOp::Empty, RingOp::Full, RingOp::Size,
                      RingOp::OverrunCounter, RingOp::ResetOverrunCounter}) {
        keep.clear();
        for (const auto &r : roles_)
            if (expected(op, r[0], r[1], r[2]) == gotClass) keep.push_back(r);
        if (!keep.empty()) {
            found = op;
            break;
        }
    }
    if (found == RingOp::None) {
        reason = "computes none of a ring's operations (" + got + ")";
        return done_[f] = RingOp::None;
    }
    roles_ = keep;
    return done_[f] = found;
}

// ---------------------------------------------------------------------------
// Value sets. Every step passes a value on unchanged (a store, a load, an
// argument, a widening), so a set is the union of what flows in, and a
// query that comes back to a node still being computed may take that
// node's set as empty: the root's union is the same (a member copied from
// another object of its type, as a copy constructor does, adds nothing).

ValueSets::ValueSets(ModuleOp module, Elsewhere elsewhere) : module_(module), elsewhere_(std::move(elsewhere)) {
    for (cir::FuncOp f : module.getOps<cir::FuncOp>())
        if (f.getSymName() == "main" && !f.getBody().empty()) closed_ = true;
    module.walk([&](cir::CallOp c) {
        if (cir::FuncOp g = lookupCallee(c)) callers_[g].push_back(c);
    });
    // A function whose symbol is used other than as a direct callee may be
    // called from where the module does not show.
    if (auto uses = SymbolTable::getSymbolUses(module.getOperation()))
        for (const SymbolTable::SymbolUse &u : *uses) {
            if (auto c = dyn_cast<cir::CallOp>(u.getUser()); c && c.getCalleeAttr() == u.getSymbolRef()) continue;
            if (Operation *def = SymbolTable::lookupNearestSymbolFrom(module, u.getSymbolRef()))
                escaping_.insert(def);
        }
}

std::optional<ValueSets::Set> ValueSets::of(Value v) {
    why_.clear();
    return of(v, 0);
}

std::nullopt_t ValueSets::unknown(Operation *at, const Twine &what) {
    if (why_.empty()) {
        auto f = at ? at->getParentOfType<cir::FuncOp>() : cir::FuncOp();
        why_ = (what + (f ? " in " + llvm::demangle(f.getSymName().str()) : std::string())).str();
    }
    return std::nullopt;
}

std::optional<ValueSets::Set> ValueSets::of(Value v, unsigned depth) {
    if (!closed_) return unknown(nullptr, "the module has no main");
    if (depth > 160) return unknown(v.getDefiningOp(), "a value too far to follow");
    if (auto c = v.getDefiningOp<cir::ConstantOp>()) {
        if (auto i = dyn_cast<cir::IntAttr>(c.getValue())) return Set{i.getValue().getSExtValue()};
        if (auto b = dyn_cast<cir::BoolAttr>(c.getValue())) return Set{b.getValue() ? 1 : 0};
        return unknown(c, "a constant that is not an integer");
    }
    if (auto c = v.getDefiningOp<cir::CastOp>()) {
        if (c.getKind() == cir::CastKind::integral || c.getKind() == cir::CastKind::bool_to_int) return of(c.getSrc(), depth + 1);
        return unknown(c, "a conversion");
    }
    if (auto ld = v.getDefiningOp<cir::LoadOp>()) return pointee(ld.getAddr(), depth + 1);
    if (auto arg = dyn_cast<BlockArgument>(v)) {
        auto f = dyn_cast<cir::FuncOp>(arg.getOwner()->getParentOp());
        if (!f || arg.getOwner() != &f.getBody().front()) return unknown(nullptr, "a block argument");
        return param(f, arg.getArgNumber(), depth + 1);
    }
    return unknown(v.getDefiningOp(), "a value computed by " + v.getDefiningOp()->getName().getStringRef());
}

std::optional<ValueSets::Set> ValueSets::pointee(Value ptr, unsigned depth) {
    if (depth > 160) return unknown(ptr.getDefiningOp(), "a pointer too far to follow");
    ptr = traceToLocalRoot(ptr);
    // std::move and std::forward hand back their argument.
    if (auto c = ptr.getDefiningOp<cir::CallOp>()) {
        cir::FuncOp g = lookupCallee(c);
        if (g && c.getNumOperands() == 1 && isStdForward(nameOf(g.getSymName()))) return pointee(c.getOperand(0), depth + 1);
        return unknown(c, "a pointer a call returns");
    }
    if (auto a = ptr.getDefiningOp<cir::AllocaOp>()) return slot(a, depth + 1);
    if (auto gm = ptr.getDefiningOp<cir::GetMemberOp>()) {
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
        if (!rec) return unknown(gm, "a member of something that is not a record");
        return member(rec, gm.getIndex(), depth + 1);
    }
    // A reference parameter, spilled to its slot and loaded back.
    if (auto ld = ptr.getDefiningOp<cir::LoadOp>()) {
        Value root = traceToRoot(ld.getResult());
        if (auto arg = dyn_cast<BlockArgument>(root)) {
            auto f = dyn_cast<cir::FuncOp>(arg.getOwner()->getParentOp());
            if (f && arg.getOwner() == &f.getBody().front()) return paramPointee(f, arg.getArgNumber(), depth + 1);
        }
        return unknown(ld, "a pointer loaded from memory");
    }
    if (auto arg = dyn_cast<BlockArgument>(ptr)) {
        auto f = dyn_cast<cir::FuncOp>(arg.getOwner()->getParentOp());
        if (f && arg.getOwner() == &f.getBody().front()) return paramPointee(f, arg.getArgNumber(), depth + 1);
    }
    return unknown(ptr.getDefiningOp(), "a pointer the pass cannot follow");
}

// Every store into the slot, if its address goes only to its own loads and
// stores and to parameters that only read through it.
std::optional<ValueSets::Set> ValueSets::slot(cir::AllocaOp a, unsigned depth) {
    if (!inProgress_.insert({a, 0}).second) return Set{};  // a cycle adds no value of its own
    std::optional<Set> out = Set{};
    for (Operation *u : a->getUsers()) {
        if (auto ld = dyn_cast<cir::LoadOp>(u); ld && ld.getAddr() == a.getResult()) continue;
        if (auto st = dyn_cast<cir::StoreOp>(u); st && st.getAddr() == a.getResult() && st.getValue() != a.getResult()) {
            auto s = of(st.getValue(), depth + 1);
            if (!s) {
                out.reset();
                break;
            }
            out->insert(s->begin(), s->end());
            continue;
        }
        if (auto c = dyn_cast<cir::CallOp>(u)) {
            cir::FuncOp g = lookupCallee(c);
            bool ok = static_cast<bool>(g);
            for (unsigned i = 0; ok && i < c.getNumOperands(); ++i)
                if (c.getOperand(i) == a.getResult()) ok = readOnlyParam(g, i, 0);
            if (ok) continue;
        }
        unknown(u, "a local whose address goes to " + u->getName().getStringRef());
        out.reset();
        break;
    }
    inProgress_.erase({a, 0});
    if (out && out->empty()) return unknown(a, "a local never written");  // unknown, not "no value"
    return out;
}

std::optional<ValueSets::Set> ValueSets::param(cir::FuncOp f, unsigned i, unsigned depth) {
    if (escaping_.contains(f)) return unknown(nullptr, "a parameter of " + llvm::demangle(f.getSymName().str()) + ", whose address is taken");
    if (elsewhere_.calls && elsewhere_.calls(f))
        return unknown(nullptr, "a parameter of " + llvm::demangle(f.getSymName().str()) + ", which another file may call");
    if (!inProgress_.insert({f, i}).second) return Set{};
    std::optional<Set> out = Set{};
    for (cir::CallOp c : callers_.lookup(f)) {
        if (i >= c.getNumOperands()) {
            out.reset();
            break;
        }
        auto s = of(c.getOperand(i), depth + 1);
        if (!s) {
            out.reset();
            break;
        }
        out->insert(s->begin(), s->end());
    }
    inProgress_.erase({f, i});
    return out;
}

std::optional<ValueSets::Set> ValueSets::paramPointee(cir::FuncOp f, unsigned i, unsigned depth) {
    if (escaping_.contains(f)) return unknown(nullptr, "a parameter of " + llvm::demangle(f.getSymName().str()) + ", whose address is taken");
    if (elsewhere_.calls && elsewhere_.calls(f))
        return unknown(nullptr, "a parameter of " + llvm::demangle(f.getSymName().str()) + ", which another file may call");
    if (!readOnlyParam(f, i, 0))
        return unknown(nullptr, "what parameter " + Twine(i) + " of " + llvm::demangle(f.getSymName().str()) +
                                    " points to, which it may write (" + why_ + ")");
    if (!inProgress_.insert({f, i + 1000}).second) return Set{};
    std::optional<Set> out = Set{};
    for (cir::CallOp c : callers_.lookup(f)) {
        if (i >= c.getNumOperands()) {
            out.reset();
            break;
        }
        auto s = pointee(c.getOperand(i), depth + 1);
        if (!s) {
            out.reset();
            break;
        }
        out->insert(s->begin(), s->end());
    }
    inProgress_.erase({f, i + 1000});
    return out;
}

// Every store into the member, anywhere, if every address of it goes only
// to loads and stores.
std::optional<ValueSets::Set> ValueSets::member(cir::RecordType rec, unsigned i, unsigned depth) {
    const std::pair<const void *, unsigned> key{rec.getAsOpaquePointer(), i};
    if (const std::string &how = exposed(rec); !how.empty())
        return unknown(nullptr, "a member of " + recordName(rec) + ", whose storage is " + how);
    if (!membersInProgress_.insert(key).second) return Set{};
    std::optional<Set> out = Set{};
    bool any = false;
    module_.walk([&](cir::GetMemberOp gm) {
        if (!out) return WalkResult::interrupt();
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        if (!pt || pt.getPointee() != rec || gm.getIndex() != i) return WalkResult::advance();
        for (Operation *u : gm->getUsers()) {
            if (auto ld = dyn_cast<cir::LoadOp>(u); ld && ld.getAddr() == gm.getResult()) continue;
            if (auto st = dyn_cast<cir::StoreOp>(u); st && st.getAddr() == gm.getResult()) {
                any = true;
                auto s = of(st.getValue(), depth + 1);
                if (!s) {
                    out.reset();
                    return WalkResult::interrupt();
                }
                out->insert(s->begin(), s->end());
                continue;
            }
            unknown(u, "a member whose address goes to " + u->getName().getStringRef());
            out.reset();
            return WalkResult::interrupt();
        }
        return WalkResult::advance();
    });
    // The whole object copied brings no new value: what it copies came
    // from these stores too (exposed() rules out every other way in).
    membersInProgress_.erase(key);
    if (!any) return unknown(nullptr, "a member never stored");
    return out;
}

bool ValueSets::contains(Type t, cir::RecordType rec, unsigned depth) {
    if (t == rec) return true;
    if (depth > 32) return true;  // too deep to tell: as if it did
    if (auto a = dyn_cast<cir::ArrayType>(t)) return contains(a.getElementType(), rec, depth + 1);
    if (auto r = dyn_cast<cir::RecordType>(t); r && !r.isIncomplete())
        for (Type m : r.getMembers())
            if (contains(m, rec, depth + 1)) return true;
    return false;
}

// The values of a member are the values stored into it member by member
// only if nothing reaches an object's storage another way. Every pointer
// to a type that holds the record (the record, a record or array with it
// inside) is followed to its uses: member addresses, loads, the pointer
// stored or returned or passed to a function whose body is here (whose
// parameter is such a pointer in turn), a base or derived class's address,
// a step along an array. Anything else exposes the storage: the object
// written whole other than from another such object (value-initialised to
// zero, say), the pointer cast to another type (memcpy and memset take
// void *, a pointer to member is an offset from a byte pointer), handed to
// a function with no body here, or a pointer of another type cast to it,
// which is how raw bytes become an object without a constructor, unless
// the record has a destructor and so begins every life in a constructor.
// A global of such a type with an initialiser, and a record another file
// names, expose it too.
const std::string &ValueSets::exposed(cir::RecordType rec) {
    auto [it, fresh] = exposed_.try_emplace(rec.getAsOpaquePointer());
    if (!fresh) return it->second;
    std::string why;
    auto expose = [&](Operation *at, const Twine &what) {
        if (!why.empty()) return;
        auto f = at ? at->getParentOfType<cir::FuncOp>() : cir::FuncOp();
        why = (what + (f ? " in " + llvm::demangle(f.getSymName().str()) : std::string())).str();
    };
    if (elsewhere_.names && elsewhere_.names(rec)) expose(nullptr, "named by another file");
    bool hasDtor = false;
    for (cir::FuncOp f : module_.getOps<cir::FuncOp>())
        if (f.isCxxDestructor() && f.getNumArguments() >= 1)
            if (auto pt = dyn_cast<cir::PointerType>(f.getArgumentTypes()[0]); pt && pt.getPointee() == rec)
                hasDtor = true;
    auto holds = [&](Type t) {
        auto pt = dyn_cast<cir::PointerType>(t);
        return pt && contains(pt.getPointee(), rec);
    };
    auto deallocates = [](cir::FuncOp g) {
        StringRef n = g ? g.getSymName() : StringRef();
        return n.starts_with("_ZdlPv") || n.starts_with("_ZdaPv") || n == "_ZnwmPv" || n == "_ZnamPv";
    };
    auto check = [&](Value v) {
        for (OpOperand &use : v.getUses()) {
            Operation *u = use.getOwner();
            // Addresses inside the object (a member, the vtable pointer, a
            // base, an element), reads, comparisons, and the pointer handed
            // on as a value, which is followed where it arrives.
            if (isa<cir::GetMemberOp, cir::LoadOp, cir::ReturnOp, cir::YieldOp, cir::CmpOp, cir::BaseClassAddrOp,
                    cir::DerivedClassAddrOp, cir::PtrStrideOp, cir::GetElementOp, cir::VTableGetVPtrOp,
                    cir::LifetimeStartOp, cir::LifetimeEndOp>(u))
                continue;
            if (auto st = dyn_cast<cir::StoreOp>(u)) {
                if (st.getValue() == v) continue;  // the pointer kept in a slot, read back as such
                if (st.getValue().getDefiningOp<cir::LoadOp>()) continue;  // a copy of another such object
                return expose(u, "written whole other than from another object");
            }
            if (auto cp = dyn_cast<cir::CopyOp>(u)) {
                if (holds(cp.getDst().getType()) && holds(cp.getSrc().getType())) continue;
                return expose(u, "copied as bytes");
            }
            if (auto call = dyn_cast<cir::CallOp>(u)) {
                cir::FuncOp g = lookupCallee(call);
                if (g && !g.getBody().empty() && !call.isIndirect()) continue;
                return expose(u, "handed to " + (g ? llvm::demangle(g.getSymName().str()) : std::string("an indirect call")) +
                                     ", which has no body here");
            }
            if (auto c = dyn_cast<cir::CastOp>(u)) {
                if (holds(c.getResult().getType()) || c.getKind() == cir::CastKind::ptr_to_bool) continue;
                // The object's address handed to operator delete, or to
                // placement new, which constructs another object there.
                if (llvm::all_of(c->getUsers(), [&](Operation *cu) {
                        auto cc = dyn_cast<cir::CallOp>(cu);
                        return cc && deallocates(lookupCallee(cc));
                    }))
                    continue;
                return expose(u, "reached through a pointer of another type");
            }
            return expose(u, "reached by " + u->getName().getStringRef());
        }
    };
    module_.walk([&](Operation *op) {
        if (!why.empty()) return WalkResult::interrupt();
        if (auto g = dyn_cast<cir::GlobalOp>(op); g && contains(g.getSymType(), rec) && g.getInitialValue())
            expose(op, "initialised by a global's initialiser");
        if (auto c = dyn_cast<cir::CastOp>(op); c && holds(c.getResult().getType()) && !holds(c.getSrc().getType()) &&
                                                  !hasDtor)
            expose(op, "made from a pointer of another type, and the record has no destructor to show that "
                       "its objects begin in a constructor");
        for (Value r : op->getResults())
            if (holds(r.getType())) check(r);
        for (Region &region : op->getRegions())
            for (Block &b : region)
                for (BlockArgument a : b.getArguments())
                    if (holds(a.getType())) check(a);
        return WalkResult::advance();
    });
    it->second = why;
    return it->second;
}

// Whether a callee only reads through its reference parameter: loads,
// std::move/std::forward of it, and passing it on to parameters that only
// read.
bool ValueSets::readOnlyParam(cir::FuncOp f, unsigned i, unsigned depth) {
    if (f && f.getBody().empty())
        return unknown(nullptr, "a reference passed to " + llvm::demangle(f.getSymName().str()) + ", which has no body here"), false;
    if (!f || i >= f.getNumArguments()) return false;
    if (depth > 48) return unknown(nullptr, "a reference passed on too far to follow"), false;
    if (!inProgress_.insert({f, i + 2000}).second) return true;  // a cycle adds no write
    Value arg = f.getBody().front().getArgument(i);
    bool ok = readOnlyUses(arg, depth + 1);
    inProgress_.erase({f, i + 2000});
    return ok;
}

bool ValueSets::readOnlyUses(Value ptr, unsigned depth) {
    if (depth > 48) return false;
    for (OpOperand &use : ptr.getUses()) {
        Operation *u = use.getOwner();
        if (auto ld = dyn_cast<cir::LoadOp>(u); ld && ld.getAddr() == ptr) continue;
        // The parameter spilled to its slot: what is loaded back is the
        // same pointer.
        if (auto st = dyn_cast<cir::StoreOp>(u); st && st.getValue() == ptr) {
            auto a = st.getAddr().getDefiningOp<cir::AllocaOp>();
            if (!a) return false;
            for (Operation *au : a->getUsers()) {
                if (auto s2 = dyn_cast<cir::StoreOp>(au); s2 && s2.getAddr() == a.getResult()) {
                    if (s2.getValue() != ptr) return false;
                    continue;
                }
                auto l2 = dyn_cast<cir::LoadOp>(au);
                if (!l2 || l2.getAddr() != a.getResult() || !readOnlyUses(l2.getResult(), depth + 1)) return false;
            }
            continue;
        }
        if (auto c = dyn_cast<cir::CallOp>(u)) {
            cir::FuncOp g = lookupCallee(c);
            if (!g) return unknown(c, "a reference passed to an indirect call"), false;
            if (isStdForward(nameOf(g.getSymName())) && c.getNumOperands() == 1) {
                if (!readOnlyUses(c.getResult(), depth + 1)) return false;
                continue;
            }
            if (!readOnlyParam(g, use.getOperandNumber(), depth + 1)) return false;
            continue;
        }
        if (auto cast = dyn_cast<cir::CastOp>(u); cast && cast.getKind() == cir::CastKind::bitcast) {
            if (!readOnlyUses(cast.getResult(), depth + 1)) return false;
            continue;
        }
        unknown(u, "a reference that goes to " + u->getName().getStringRef());
        return false;
    }
    return true;
}

// The truth of a condition over the value sets of what it compares:
// decided only when every combination gives the same answer.
std::optional<bool> ValueSets::truth(Value cond, unsigned depth, bool &decided) {
    decided = false;
    if (auto n = cond.getDefiningOp<cir::NotOp>()) {
        auto t = truth(n.getInput(), depth + 1, decided);
        return t ? std::optional<bool>(!*t) : std::nullopt;
    }
    if (auto c = cond.getDefiningOp<cir::CastOp>(); c && c.getKind() == cir::CastKind::int_to_bool) {
        auto s = of(c.getSrc(), depth + 1);
        if (!s || s->empty()) return std::nullopt;
        const bool any0 = s->count(0), anyN = s->size() > (any0 ? 1u : 0u);
        if (any0 && anyN) return std::nullopt;
        decided = true;
        return anyN;
    }
    auto cmp = cond.getDefiningOp<cir::CmpOp>();
    if (!cmp) return std::nullopt;
    auto l = of(cmp.getLhs(), depth + 1), r = of(cmp.getRhs(), depth + 1);
    if (!l || !r || l->empty() || r->empty()) return std::nullopt;
    std::optional<bool> all;
    for (int64_t a : *l)
        for (int64_t b : *r) {
            bool v;
            switch (cmp.getKind()) {
            case cir::CmpOpKind::eq: v = a == b; break;
            case cir::CmpOpKind::ne: v = a != b; break;
            case cir::CmpOpKind::lt: v = a < b; break;
            case cir::CmpOpKind::le: v = a <= b; break;
            case cir::CmpOpKind::gt: v = a > b; break;
            case cir::CmpOpKind::ge: v = a >= b; break;
            default: return std::nullopt;
            }
            if (all && *all != v) return std::nullopt;
            all = v;
        }
    decided = all.has_value();
    return all;
}

bool ValueSets::neverRuns(Operation *op, std::string &why) {
    if (!closed_) {
        why = "the module has no main, so its functions may be called from elsewhere";
        return false;
    }
    for (Operation *child = op, *p = op->getParentOp(); p && !isa<cir::FuncOp>(p); child = p, p = p->getParentOp()) {
        auto i = dyn_cast<cir::IfOp>(p);
        if (!i) continue;
        const bool inThen = i.getThenRegion().isAncestor(child->getParentRegion());
        bool decided = false;
        why_.clear();
        std::optional<bool> t = truth(i.getCondition(), 0, decided);
        if (decided && t && *t != inThen) {
            why = "the condition around it is always " + std::string(*t ? "true" : "false");
            return true;
        }
    }
    why = "no condition around it is decided by the values its operands may hold";
    if (!why_.empty()) why += "; one of them is unknown: " + why_;
    return false;
}
