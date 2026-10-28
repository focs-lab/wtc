#include "WTC/Transforms/WorkEstimate.h"
#include "WTC/Transforms/Blocking.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCOps.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Demangle/Demangle.h"

using namespace mlir;
using namespace mlir::wtc;

namespace {

// Units of the operations that are not one each (WorkEstimate.h).
constexpr double kDivision = 40, kDivisionByConstant = 2, kFloatDivision = 15, kTranscendental = 200,
                 kTimestamp = 90, kAllocator = 100, kCall = 1;

// A constant integer, through integral casts and globals declared constant.
std::optional<llvm::APInt> constantInt(Value v) {
    for (int hops = 0; hops < 8 && v; ++hops) {
        Operation *def = v.getDefiningOp();
        if (!def) return std::nullopt;
        if (auto c = dyn_cast<cir::ConstantOp>(def)) {
            if (auto i = dyn_cast<cir::IntAttr>(c.getValue())) return i.getValue();
            return std::nullopt;
        }
        if (auto cast = dyn_cast<cir::CastOp>(def); cast && cast.getKind() == cir::CastKind::integral) {
            auto from = dyn_cast<cir::IntType>(cast.getSrc().getType());
            auto to = dyn_cast<cir::IntType>(cast.getType());
            auto inner = constantInt(cast.getSrc());
            if (!from || !to || !inner) return std::nullopt;
            return from.isSigned() ? inner->sextOrTrunc(to.getWidth()) : inner->zextOrTrunc(to.getWidth());
        }
        if (auto ld = dyn_cast<cir::LoadOp>(def)) {
            auto gg = ld.getAddr().getDefiningOp<cir::GetGlobalOp>();
            if (!gg || ld.getIsVolatile()) return std::nullopt;
            auto g = dyn_cast_or_null<cir::GlobalOp>(
                SymbolTable::lookupNearestSymbolFrom(ld, gg.getNameAttr()));
            if (!g || !g.getConstant()) return std::nullopt;
            auto init = g.getInitialValue();
            if (!init) return std::nullopt;
            if (auto i = dyn_cast<cir::IntAttr>(*init)) return i.getValue();
            return std::nullopt;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool signedInt(Type t) {
    auto i = dyn_cast<cir::IntType>(t);
    return i && i.isSigned();
}

std::string nameOf(cir::FuncOp f) { return llvm::demangle(f.getSymName().str()); }

} // namespace

namespace mlir::wtc {

std::optional<uint64_t> constantTripCount(cir::ForOp loop) {
    Region &cond = loop.getCond();
    if (!cond.hasOneBlock()) return std::nullopt;
    auto term = dyn_cast<cir::ConditionOp>(cond.front().getTerminator());
    auto cmp = term ? term.getCondition().getDefiningOp<cir::CmpOp>() : cir::CmpOp();
    if (!cmp || !isa<cir::IntType>(cmp.getLhs().getType())) return std::nullopt;
    const bool isSigned = signedInt(cmp.getLhs().getType());

    // i on one side, a constant on the other; read as `i <kind> bound`.
    cir::CmpOpKind kind = cmp.getKind();
    cir::LoadOp ind = cmp.getLhs().getDefiningOp<cir::LoadOp>();
    std::optional<llvm::APInt> bound = constantInt(cmp.getRhs());
    if (!ind || !bound) {
        ind = cmp.getRhs().getDefiningOp<cir::LoadOp>();
        bound = constantInt(cmp.getLhs());
        if (!ind || !bound) return std::nullopt;
        switch (kind) {
        case cir::CmpOpKind::lt: kind = cir::CmpOpKind::gt; break;
        case cir::CmpOpKind::le: kind = cir::CmpOpKind::ge; break;
        case cir::CmpOpKind::gt: kind = cir::CmpOpKind::lt; break;
        case cir::CmpOpKind::ge: kind = cir::CmpOpKind::le; break;
        default: break;
        }
    }
    auto slot = ind.getAddr().getDefiningOp<cir::AllocaOp>();
    if (!slot) return std::nullopt;

    // i is read and written directly and nowhere else, and inside the loop
    // only the step writes it, once, by one up or down.
    int direction = 0;
    for (Operation *u : slot->getUsers()) {
        if (isa<cir::LoadOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
        auto st = dyn_cast<cir::StoreOp>(u);
        if (!st || st.getAddr() != slot.getResult()) return std::nullopt;
        if (!loop->isAncestor(u)) continue;
        if (direction != 0 || !loop.getStep().isAncestor(u->getParentRegion())) return std::nullopt;
        auto loadsI = [&](Value x) {
            auto l = x.getDefiningOp<cir::LoadOp>();
            return l && l.getAddr() == slot.getResult();
        };
        Value v = st.getValue();
        if (auto inc = v.getDefiningOp<cir::IncOp>(); inc && loadsI(inc.getInput())) direction = 1;
        else if (auto dec = v.getDefiningOp<cir::DecOp>(); dec && loadsI(dec.getInput())) direction = -1;
        else if (auto add = v.getDefiningOp<cir::AddOp>(); add && loadsI(add.getLhs())) {
            auto k = constantInt(add.getRhs());
            if (k && k->isOne()) direction = 1;
            else if (k && k->isAllOnes()) direction = -1;
        } else if (auto sub = v.getDefiningOp<cir::SubOp>(); sub && loadsI(sub.getLhs())) {
            auto k = constantInt(sub.getRhs());
            if (k && k->isOne()) direction = -1;
        }
        if (direction == 0) return std::nullopt;
    }
    if (direction == 0) return std::nullopt;

    // Where it starts: the nearest store to i before the loop in its block,
    // of a constant, with nothing in between that writes i.
    std::optional<llvm::APInt> first;
    for (Operation *op = loop->getPrevNode(); op && !first; op = op->getPrevNode()) {
        if (auto st = dyn_cast<cir::StoreOp>(op); st && st.getAddr() == slot.getResult()) {
            first = constantInt(st.getValue());
            if (!first) return std::nullopt;
            break;
        }
        bool writes = false;
        op->walk([&](cir::StoreOp st) { writes |= st.getAddr() == slot.getResult(); });
        if (writes) return std::nullopt;
    }
    if (!first || first->getBitWidth() > 64 || bound->getBitWidth() > 64) return std::nullopt;

    // A bound i reaches only by wrapping -- i <= the type's largest value,
    // i >= its smallest -- never ends the loop.
    const unsigned width = cast<cir::IntType>(cmp.getLhs().getType()).getWidth();
    if (bound->getBitWidth() == width) {
        const bool atMax = isSigned ? bound->isMaxSignedValue() : bound->isMaxValue();
        const bool atMin = isSigned ? bound->isMinSignedValue() : bound->isMinValue();
        if ((direction > 0 && kind == cir::CmpOpKind::le && atMax) ||
            (direction < 0 && kind == cir::CmpOpKind::ge && atMin))
            return std::nullopt;
    }

    // In 128 bits, so that no 64-bit pair of first and bound overflows.
    auto wide = [&](const llvm::APInt &v) -> __int128 {
        return isSigned ? static_cast<__int128>(v.getSExtValue()) : static_cast<__int128>(v.getZExtValue());
    };
    const __int128 a = wide(*first), b = wide(*bound);
    __int128 n;
    if (direction > 0) {
        switch (kind) {
        case cir::CmpOpKind::lt: n = b - a; break;
        case cir::CmpOpKind::le: n = b - a + 1; break;
        case cir::CmpOpKind::ne:
            if (b < a) return std::nullopt;  // runs until i wraps
            n = b - a;
            break;
        default: return std::nullopt;
        }
    } else {
        switch (kind) {
        case cir::CmpOpKind::gt: n = a - b; break;
        case cir::CmpOpKind::ge: n = a - b + 1; break;
        case cir::CmpOpKind::ne:
            if (b > a) return std::nullopt;
            n = a - b;
            break;
        default: return std::nullopt;
        }
    }
    if (n < 0) n = 0;
    if (n > static_cast<__int128>(UINT64_MAX)) return std::nullopt;
    return static_cast<uint64_t>(n);
}

WorkEstimate WorkEstimator::region(Region &r, Skip skip) {
    if (r.empty()) return {};
    if (!r.hasOneBlock())
        return WorkEstimate::unbounded(r.getParentOp(), "its control flow is not structured (a goto)");
    WorkEstimate e;
    for (Operation &o : r.front()) {
        if (skip && skip(&o)) continue;
        e += op(&o, skip);
        if (!e.bounded) break;
    }
    return e;
}

WorkEstimate WorkEstimator::iteration(cir::LoopOpInterface loop, Skip skip) {
    WorkEstimate e = region(loop.getCond(), skip);
    e += region(loop.getBody(), skip);
    if (Region *step = loop.maybeGetStep()) e += region(*step, skip);
    return e;
}

WorkEstimate WorkEstimator::op(Operation *o, Skip skip) {
    if (auto loop = dyn_cast<cir::ForOp>(o)) {
        std::optional<uint64_t> trips = constantTripCount(loop);
        if (!trips) return WorkEstimate::unbounded(o, "it runs a loop whose trip count is not known here");
        WorkEstimate e = region(loop.getCond(), skip);
        e += iteration(loop, skip).times(*trips);
        return e;
    }
    if (isa<cir::LoopOpInterface>(o))
        return WorkEstimate::unbounded(o, "it runs a loop whose trip count is not known here");
    if (auto i = dyn_cast<cir::IfOp>(o))
        return WorkEstimate::larger(region(i.getThenRegion(), skip), region(i.getElseRegion(), skip));
    if (auto t = dyn_cast<cir::TernaryOp>(o))
        return WorkEstimate::larger(region(t.getTrueRegion(), skip), region(t.getFalseRegion(), skip));
    if (auto c = dyn_cast<cir::CallOp>(o)) return call(c);
    if (isa<WTCDialect>(o->getDialect()))
        return WorkEstimate::unbounded(o, "it acts on a queue or a mutex, whose cost is not modelled");

    // Fixed-length memory operations.
    StringRef n = o->getName().getStringRef();
    if (n == "cir.libc.memcpy" || n == "cir.libc.memmove" || n == "cir.libc.memset" || n == "cir.libc.memchr") {
        auto len = constantInt(o->getOperands().back());
        if (!len || len->getActiveBits() > 40)
            return WorkEstimate::unbounded(o, "it copies or scans a length not known here");
        return WorkEstimate::of(2 + static_cast<double>(len->getZExtValue()) / 8);
    }
    if (auto copy = dyn_cast<cir::CopyOp>(o)) {
        uint64_t bytes = layout.getTypeSize(copy.getDst().getType().getPointee()).getFixedValue();
        return WorkEstimate::of(2 + static_cast<double>(bytes) / 8);
    }

    WorkEstimate e = WorkEstimate::of(own(o));
    for (Region &r : o->getRegions()) {
        e += region(r, skip);
        if (!e.bounded) break;
    }
    return e;
}

double WorkEstimator::own(Operation *o) {
    static const llvm::StringSet<> free = {
        "cir.const",          "cir.alloca",          "cir.get_global",   "cir.get_member",
        "cir.yield",          "cir.condition",       "cir.return",       "cir.break",
        "cir.continue",       "cir.scope",           "cir.cleanup.scope", "cir.case",
        "cir.switch",         "cir.lifetime.start",  "cir.lifetime.end", "cir.assume",
        "cir.expect",         "cir.unreachable",     "cir.base_class_addr", "cir.derived_class_addr",
        "cir.launder",        "cir.objsize",         "cir.is_constant",  "cir.freeze",
        "cir.token.none",     "cir.label",           "cir.br",           "cir.try",
        "builtin.unrealized_conversion_cast"};
    static const llvm::StringSet<> transcendental = {
        "cir.sin",  "cir.cos",  "cir.tan",   "cir.asin",  "cir.acos", "cir.atan", "cir.atan2", "cir.sinh",
        "cir.cosh", "cir.tanh", "cir.exp",   "cir.exp2",  "cir.exp10", "cir.log", "cir.log2", "cir.log10",
        "cir.pow",  "cir.fmod"};
    StringRef n = o->getName().getStringRef();
    if (free.contains(n)) return 0;
    if (auto cast = dyn_cast<cir::CastOp>(o)) {
        switch (cast.getKind()) {
        case cir::CastKind::int_to_float:
        case cir::CastKind::float_to_int:
        case cir::CastKind::floating:
        case cir::CastKind::bool_to_float:
            return 1;
        default:
            return 0;
        }
    }
    if (n == "cir.div" || n == "cir.rem")
        return constantInt(o->getOperand(1)) ? kDivisionByConstant : kDivision;
    if (n == "cir.fdiv" || n == "cir.frem" || n == "cir.sqrt") return kFloatDivision;
    if (transcendental.contains(n)) return kTranscendental;
    if (auto intr = dyn_cast<cir::LLVMIntrinsicCallOp>(o)) {
        StringRef name = intr.getIntrinsicName();
        return name == "x86.rdtsc" || name == "x86.rdtscp" ? kTimestamp : 1;
    }
    if (auto asmOp = dyn_cast<cir::InlineAsmOp>(o)) {
        StringRef text = asmOp.getAsmString();
        double units = 0;
        for (size_t start = 0, i = 0; i <= text.size(); ++i)
            if (i == text.size() || text[i] == '\n' || text[i] == ';') {
                StringRef stmt = text.slice(start, i).trim();
                start = i + 1;
                if (stmt.empty()) continue;
                units += stmt.contains_insensitive("rdtsc") ? kTimestamp : 1;
            }
        return units;
    }
    return 1;
}

WorkEstimate WorkEstimator::call(cir::CallOp c) {
    llvm::SmallVector<cir::FuncOp, 4> targets;
    if (cir::FuncOp callee = lookupCallee(c)) {
        targets.push_back(callee);
    } else {
        targets = indirectCallees(c);
        if (targets.empty())
            return WorkEstimate::unbounded(c, "it makes an indirect call whose targets are not known here");
    }
    WorkEstimate worst;
    for (cir::FuncOp t : targets) {
        WorkEstimate e;
        if (t.getBody().empty() || cir::isInterposableLinkage(t.getLinkage())) {
            e = external(c, t);
        } else if (llvm::is_contained(stack, t.getOperation())) {
            e = WorkEstimate::unbounded(c, "it makes a recursive call to " + nameOf(t));
        } else if (stack.size() >= kMaxDepth) {
            metDepth = true;
            e = WorkEstimate::unbounded(c, "it calls more than " + std::to_string(kMaxDepth) + " functions deep");
        } else {
            e = function(t);
            if (!e.bounded) e = WorkEstimate::unbounded(c, "it calls " + nameOf(t) + ", where " + e.why);
        }
        worst = WorkEstimate::larger(worst, e);
        if (!worst.bounded) return worst;
    }
    worst += WorkEstimate::of(kCall);
    return worst;
}

WorkEstimate WorkEstimator::external(cir::CallOp c, cir::FuncOp f) {
    const StringRef n = f.getSymName();
    if (!f.getBody().empty())
        return WorkEstimate::unbounded(c, "it calls " + nameOf(f) + ", whose body here may be replaced at link time");
    if (touchesOnlyItsArguments(f)) {
        if (n == "abs" || n == "labs" || n == "llabs" || n == "fabs" || n == "fabsf") return WorkEstimate::of(1);
        if (n == "sqrt" || n == "sqrtf") return WorkEstimate::of(kFloatDivision);
        // memcpy, memmove, memset, memcmp, memchr and strncmp take their
        // length third.
        if (n != "strlen" && n != "strcmp" && c.getArgOperands().size() >= 3) {
            auto len = constantInt(c.getArgOperands()[2]);
            if (len && len->getActiveBits() <= 40)
                return WorkEstimate::of(2 + static_cast<double>(len->getZExtValue()) / 8);
            return WorkEstimate::unbounded(c, "it calls " + n.str() + " with a length not known here");
        }
        return WorkEstimate::unbounded(c, "it calls " + n.str() + ", whose work depends on its argument");
    }
    if (auto ext = externalBlocking(f); ext && ext->kind == BlockingKind::Allocates) return WorkEstimate::of(kAllocator);
    return WorkEstimate::unbounded(c, "it calls " + nameOf(f) + ", whose body is not visible here");
}

WorkEstimate WorkEstimator::function(cir::FuncOp f) {
    if (auto it = memo.find(f); it != memo.end()) return it->second;
    // A body is entered only from call(), which has refused recursion and
    // depth already.
    const bool outer = metDepth;
    metDepth = false;
    stack.push_back(f);
    WorkEstimate e = region(f.getBody());
    stack.pop_back();
    if (!metDepth) memo[f] = e;
    metDepth = outer || metDepth;
    return e;
}

} // namespace mlir::wtc
