#include "WTC/Progress.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace mlir::wtc {

// ---------------------------------------------------------------------------
// Count

std::string Count::str() const {
    switch (kind) {
    case Kind::One: return "1";
    case Kind::Const: return std::to_string(k);
    case Kind::Symbolic: return sym;
    case Kind::Unbounded: return "unbounded";
    }
    return "unbounded";
}

Count Count::parse(llvm::StringRef text) {
    text = text.trim();
    if (text.empty() || text == "unbounded") return unbounded();
    uint64_t v;
    if (!text.getAsInteger(10, v)) return constant(v);
    return symbolic(text.str());
}

// A symbolic operand in a compound expression is parenthesised when it is
// itself a sum.
static std::string operand(const Count &c, bool inProduct) {
    std::string s = c.str();
    if (inProduct && c.kind == Count::Kind::Symbolic && s.find(" + ") != std::string::npos) return "(" + s + ")";
    return s;
}

Count operator+(const Count &a, const Count &b) {
    if (a.isUnbounded() || b.isUnbounded()) return Count::unbounded();
    if (auto x = a.value()) {
        if (auto y = b.value()) {
            uint64_t r;
            if (__builtin_add_overflow(*x, *y, &r)) return Count::unbounded();
            return Count::constant(r);
        }
        if (*x == 0) return b;
    }
    if (auto y = b.value(); y && *y == 0) return a;
    return Count::symbolic(operand(a, false) + " + " + operand(b, false));
}

Count operator*(const Count &a, const Count &b) {
    if (auto x = a.value(); x && *x == 0) return Count::zero();
    if (auto y = b.value(); y && *y == 0) return Count::zero();
    if (a.isUnbounded() || b.isUnbounded()) return Count::unbounded();
    if (a.isOne()) return b;
    if (b.isOne()) return a;
    if (auto x = a.value())
        if (auto y = b.value()) {
            uint64_t r;
            if (__builtin_mul_overflow(*x, *y, &r)) return Count::unbounded();  // no number fits
            return Count::constant(r);
        }
    return Count::symbolic(operand(a, true) + "·" + operand(b, true));
}

// ---------------------------------------------------------------------------
// Level

llvm::StringRef stringifyLevel(Level l) {
    switch (l) {
    case Level::blocking_df: return "blocking(df)";
    case Level::blocking_sf: return "blocking(sf)";
    case Level::obstruction_free: return "obstruction_free";
    case Level::lock_free: return "lock_free";
    case Level::wait_free: return "wait_free";
    }
    return "blocking(df)";
}

llvm::StringRef describeLevel(Level l) {
    switch (l) {
    case Level::blocking_df: return "blocking (deadlock-free)";
    case Level::blocking_sf: return "blocking (starvation-free)";
    case Level::obstruction_free: return "obstruction-free";
    case Level::lock_free: return "lock-free";
    case Level::wait_free: return "wait-free";
    }
    return "blocking";
}

std::optional<Level> parseLevel(llvm::StringRef text) {
    for (Level l : {Level::blocking_df, Level::blocking_sf, Level::obstruction_free, Level::lock_free,
                    Level::wait_free})
        if (text == stringifyLevel(l)) return l;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Bound

// Arithmetic on step counts saturates: a bound that does not fit in 64
// bits is no number a check can compare, and is said to be finite only.
static bool mulOver(uint64_t a, uint64_t b, uint64_t &out) { return __builtin_mul_overflow(a, b, &out); }
static bool addOver(uint64_t a, uint64_t b, uint64_t &out) { return __builtin_add_overflow(a, b, &out); }
static Bound tooLarge() { return Bound::finite("too large to count in 64 bits"); }

Bound Bound::constant(uint64_t c, llvm::StringRef name) {
    Bound b = zero();
    if (c) b.terms.push_back(Term{c, 0, 0, 0, name.str()});
    return b;
}

Bound Bound::poly(std::initializer_list<Term> ts) {
    Bound b = zero();
    for (const Term &t : ts) b = b.plus(Bound{Kind::Poly, {t}, {}});
    return b;
}

bool Bound::dependsOnCounts() const {
    return llvm::any_of(terms, [](const Term &t) { return t.np || t.nc || t.n; });
}

std::optional<uint64_t> Bound::instantiate(const ThreadCounts &counts) const {
    if (kind != Kind::Poly) return std::nullopt;
    auto power = [](std::optional<uint64_t> base, unsigned e) -> std::optional<uint64_t> {
        if (e == 0) return 1;
        if (!base) return std::nullopt;
        uint64_t r = 1;
        for (unsigned i = 0; i < e; ++i)
            if (mulOver(r, *base, r)) return std::nullopt;
        return r;
    };
    uint64_t sum = 0;
    for (const Term &t : terms) {
        auto a = power(counts.producers.value(), t.np), b = power(counts.consumers.value(), t.nc),
             c = power(counts.domain.value(), t.n);
        if (!a || !b || !c) return std::nullopt;
        uint64_t v = t.coeff;
        if (mulOver(v, *a, v) || mulOver(v, *b, v) || mulOver(v, *c, v) || addOver(sum, v, sum)) return std::nullopt;
    }
    return sum;
}

// The weaker of two non-polynomial kinds: a history-dependent bound is no
// bound in the counts, a finite one has at least no thread stretching it.
static Bound::Kind weaker(Bound::Kind a, Bound::Kind b) {
    auto rank = [](Bound::Kind k) {
        switch (k) {
        case Bound::Kind::Poly: return 0;
        case Bound::Kind::Finite: return 1;
        case Bound::Kind::History: return 2;
        case Bound::Kind::None: return 3;
        }
        return 3;
    };
    return rank(a) >= rank(b) ? a : b;
}

static std::string joinNotes(const Bound &a, const Bound &b, Bound::Kind kind) {
    std::string n;
    for (const Bound *x : {&a, &b})
        if (x->kind == kind && !x->note.empty() && n.find(x->note) == std::string::npos)
            n += (n.empty() ? "" : "; ") + x->note;
    return n;
}

Bound Bound::plus(const Bound &o) const {
    if (kind != Kind::Poly || o.kind != Kind::Poly) {
        Kind k = weaker(kind, o.kind);
        return {k, {}, joinNotes(*this, o, k)};
    }
    Bound r = *this;
    for (const Term &t : o.terms) {
        auto it = llvm::find_if(r.terms, [&](const Term &x) { return x.sameMonomial(t); });
        if (it != r.terms.end()) {
            if (addOver(it->coeff, t.coeff, it->coeff)) return tooLarge();
        } else {
            r.terms.push_back(t);
        }
    }
    return r;
}

Bound Bound::max(const Bound &o) const {
    if (kind != Kind::Poly || o.kind != Kind::Poly) {
        Kind k = weaker(kind, o.kind);
        return {k, {}, joinNotes(*this, o, k)};
    }
    Bound r = *this;
    for (const Term &t : o.terms) {
        auto it = llvm::find_if(r.terms, [&](const Term &x) { return x.sameMonomial(t); });
        if (it != r.terms.end())
            it->coeff = std::max(it->coeff, t.coeff);
        else
            r.terms.push_back(t);
    }
    return r;
}

Bound Bound::times(const Count &trips, llvm::StringRef what) const {
    if (kind != Kind::Poly) return *this;
    if (auto k = trips.value()) {
        Bound r = *this;
        for (Term &t : r.terms)
            if (mulOver(t.coeff, *k, t.coeff)) return tooLarge();
        llvm::erase_if(r.terms, [](const Term &t) { return t.coeff == 0; });
        return r;
    }
    if (terms.empty()) return *this;
    if (trips.isUnbounded()) return none();
    return finite(("repeated " + trips.str() + " times" + (what.empty() ? "" : " (" + what + ")")).str());
}

std::string Bound::str() const {
    switch (kind) {
    case Kind::None: return "none";
    case Kind::Finite: return note.empty() ? "finite, not instantiated" : "finite, not instantiated (" + note + ")";
    case Kind::History: return note.empty() ? "not bounded in the counts" : "not bounded in the counts (" + note + ")";
    case Kind::Poly: break;
    }
    if (terms.empty()) return "0";
    // Highest degree first, then the named constants last.
    llvm::SmallVector<Term, 4> ts(terms.begin(), terms.end());
    std::stable_sort(ts.begin(), ts.end(), [](const Term &a, const Term &b) {
        const unsigned da = a.np + a.nc + a.n, db = b.np + b.nc + b.n;
        if (da != db) return da > db;
        return a.name.empty() && !b.name.empty();
    });
    std::string s;
    llvm::raw_string_ostream os(s);
    bool first = true;
    for (const Term &t : ts) {
        if (!first) os << " + ";
        first = false;
        const bool hasVar = t.np || t.nc || t.n;
        if (t.coeff != 1 || !hasVar) os << t.coeff;
        bool dot = t.coeff != 1 || !hasVar;
        auto var = [&](const char *v, uint8_t e) {
            if (!e) return;
            if (dot) os << "·";
            os << v;
            if (e == 2) os << "²";
            else if (e == 3) os << "³";
            else if (e > 3) os << "^" << unsigned(e);
            dot = true;
        };
        var("np", t.np);
        var("nc", t.nc);
        var("n", t.n);
        if (!t.name.empty()) os << " " << t.name;
    }
    return s;
}

// ---------------------------------------------------------------------------
// The other fields

std::string Alloc::str() const {
    switch (kind) {
    case Kind::never: return "never";
    case Kind::first_use: return "on first use";
    case Kind::amortised: return every ? "once every " + std::to_string(every) : "amortised";
    case Kind::always: return "may";
    }
    return "may";
}

llvm::StringRef stringifyKernel(Kernel k) {
    switch (k) {
    case Kernel::none: return "none";
    case Kernel::futex_wake: return "futex wake";
    case Kernel::futex_wait: return "futex wait";
    }
    return "none";
}

std::string describeFirstUse(unsigned flags) {
    llvm::SmallVector<std::string, 2> parts;
    if (flags & first_use_tls) parts.push_back("per-thread record");
    if (flags & first_use_guard) parts.push_back("static-initialisation guard");
    if (parts.empty()) return "none";
    return llvm::join(parts, ", ");
}

std::string describeElementCode(unsigned flags) {
    llvm::SmallVector<std::string, 4> parts;
    if ((flags & element_copy_ctor) && (flags & element_move_ctor))
        parts.push_back("copy or move constructor");
    else if (flags & element_copy_ctor)
        parts.push_back("copy constructor");
    else if (flags & element_move_ctor)
        parts.push_back("move constructor");
    if (flags & element_move_assign) parts.push_back("move-assignment");
    if (flags & element_dtor) parts.push_back("destructor");
    if (parts.empty()) return "none";
    return llvm::join(parts, ", ");
}

std::string ProgressSpec::str(const ThreadCounts *counts) const {
    std::string s;
    llvm::raw_string_ostream os(s);
    if (partial) os << "partial(";
    os << stringifyLevel(level);
    if (partial) os << ")";
    if (level == Level::wait_free) {
        os << ", bound " << bound.str();
        if (counts && bound.dependsOnCounts())
            if (auto v = bound.instantiate(*counts)) os << " = " << *v;
        if (amortised.kind != Bound::Kind::None) {
            os << ", amortised " << amortised.str();
            if (counts && amortised.dependsOnCounts())
                if (auto v = amortised.instantiate(*counts)) os << " = " << *v;
        }
    }
    if (dependent()) os << ", scheduler-dependent";
    os << "; allocates: " << alloc.str() << "; kernel: " << stringifyKernel(kernel)
       << "; first use: " << describeFirstUse(firstUse) << "; element: " << describeElementCode(element);
    return s;
}

// ---------------------------------------------------------------------------
// Forms

llvm::StringRef stringifyOpForm(OpForm f) {
    switch (f) {
    case OpForm::push: return "push";
    case OpForm::push_deferred: return "push_deferred";
    case OpForm::batch_begin: return "batch_begin";
    case OpForm::batch_end: return "batch_end";
    case OpForm::pop_shared: return "pop";
    case OpForm::pop_transfer: return "pop transfer";
    case OpForm::pop_wait: return "pop_wait";
    case OpForm::pop_wait_for: return "pop_wait_for";
    case OpForm::bounded_try_push: return "bounded try_push";
    case OpForm::bounded_push_wait: return "bounded push_wait";
    case OpForm::bounded_push_overrun: return "bounded push_overrun";
    case OpForm::bounded_push_batch: return "bounded push_batch";
    case OpForm::bounded_pop_shared: return "bounded pop";
    case OpForm::bounded_pop_transfer: return "bounded pop transfer";
    case OpForm::bounded_pop_wait: return "bounded pop_wait";
    case OpForm::bounded_pop_wait_for: return "bounded pop_wait_for";
    case OpForm::bounded_pop_batch: return "bounded pop_batch";
    case OpForm::lock: return "lock";
    case OpForm::unlock: return "unlock";
    case OpForm::lock_batch_begin: return "lock_batch_begin";
    case OpForm::lock_batch_step: return "lock_batch_step";
    case OpForm::lock_batch_end: return "lock_batch_end";
    case OpForm::delegate: return "delegate";
    }
    return "?";
}

bool isQueueForm(OpForm f) { return f < OpForm::lock; }

bool isBoundedForm(OpForm f) { return f >= OpForm::bounded_try_push && f <= OpForm::bounded_pop_batch; }

} // namespace mlir::wtc
