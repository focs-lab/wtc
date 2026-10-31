#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace mlir;
using namespace mlir::wtc;

// Queues a C program wrote itself (docs/lifting.md, "C queues"). PARSEC's
// ferret and dedup keep theirs as a struct with a ring of pointers, a
// pthread mutex, two condition variables and a count of the producers that
// have finished:
//
//     void enqueue(struct queue *q, void *v) {
//         pthread_mutex_lock(&q->mutex);
//         while (q->head == (q->tail - 1 + q->size) % q->size)
//             pthread_cond_wait(&q->full, &q->mutex);
//         q->data[q->head] = v;
//         if (++q->head == q->size) q->head = 0;
//         pthread_cond_signal(&q->empty);
//         pthread_mutex_unlock(&q->mutex);
//     }
//
// This pass finds such a struct, reads every function that takes a pointer
// to it by evaluating its body symbolically, proves that the functions are
// the operations of a bounded FIFO queue under the struct's mutex and
// nothing else, and replaces their bodies with calls into the runtime of
// shim/c_queue.h, whose functions the lifting pass raises to the dialect's
// queue operations like any shim queue's. The ownership analysis and the
// specialisation apply unchanged.
//
// The evaluation follows every path through a function, its calls into
// helpers of the program's (dedup's ringbuffer_* and queue_isTerminated)
// included, keeping the struct's members as symbols: the value a member had
// when the function took the lock, and, after a wait, the value it has when
// the wait ends, which another thread may have changed. Index arithmetic is
// put in one form, so that `if (++i == n) i = 0`, `if (i + 1 >= n) i = 0`
// and `(i + 1) % n` are one increment, and `h == (t - 1 + n) % n` and
// `(h + 1) % n == t` one test of fullness, given that the indices stay
// below the slot count, which every function maintains. What a path
// decides on is read as a truth table over the facts "empty", "full", "as
// many producers have finished as there are" and "the batch moved
// something", so that the predicates may be written either way round.
//
// The proof, conservative throughout; a remark names the reason for every
// refusal, prefixed by the condition it fails:
//
//   (a) Confinement. The module has main, and nothing it shares with other
//       units names the struct's functions or objects; the struct's
//       members are touched only by the functions that take a pointer to
//       it, each of which is one of the operations below or a helper only
//       they call; a pointer to the struct goes only to them (and to free),
//       and is kept only in locals and in globals of its own type whose
//       every use the pass sees; no global struct is initialised
//       statically; init, destroy and terminate return nothing or a
//       constant.
//   (b) The mutex is held. Every touch of a member in an operation is
//       between the lock and the unlock of the struct's mutex, on every
//       path, and a wait is given that mutex; initialisation and
//       destruction are not concurrent with the rest and take no lock.
//   (c) The mutex guards the queue and nothing else: an operation writes
//       only the ring's indices and the end count, as the operation says,
//       and outside the queue only the element a pop hands its caller; it
//       wakes no condition variable outside the queue; a batch's limit
//       reaches its comparison with every value kept.
//   (d) Waits the substitute keeps: a pop waits on its condition variable
//       while the ring is empty and not every producer has finished, a push
//       while it is full; a push wakes the variable the pops wait on, a pop
//       the one the pushes wait on, the last termination the pops' one. No
//       branch ends the program (an assertion), but init's on its own
//       allocation: the substitute makes no such check.
//   (e) FIFO: elements go in at one index and leave at the other, each
//       advanced by one modulo the slot count, which initialisation sets
//       to the allocation's size; the ring holds pointers.
//
// The operations:
//
//   init       the mutex and variables initialised, the ring allocated with
//              its slot count, both indices and the end count zero, the
//              number of producers from a parameter
//   destroy    the mutex and variables destroyed, the ring freed
//   push       wait while full; store at one index, advance it; wake
//   pop        wait while empty and not finished; if empty, return a
//              constant; else take from the other index, advance it, wake,
//              return another constant
//   terminate  the end count incremented; the pops woken when it reaches
//              the number of producers
//   batched push, batched pop
//              the same waits, then a loop that moves elements between the
//              queue's ring and a ring the caller passes, while the source
//              is not empty, the destination not full, and fewer than the
//              limit have moved; the count moved is returned
//
// A batched pop takes up to the limit adjacent elements at one step, and
// so does the runtime's, whatever the number of consumers: a batch's size
// and its tickets are one compare-and-swap (shim/c_queue.h), so two
// consumers' batches are disjoint adjacent runs, as under the mutex. The
// caller's ring must be a local of the calling function whose address is
// passed to nothing but direct calls, so that the thread that calls is the
// only one that touches it.

namespace {

// ---------------------------------------------------------------------------
// Expressions: canonical trees, compared by their printed key.

struct Node;
using Ex = std::shared_ptr<const Node>;

struct Node {
    std::string op;  // "k" a constant, "atom", or an operator
    int64_t k = 0;
    std::string name;
    std::vector<Ex> kids;
    std::string key;
};

Ex makeNode(Node n) {
    if (n.op == "k") n.key = "#" + std::to_string(n.k);
    else if (n.op == "atom") n.key = n.name;
    else {
        n.key = n.op + "(";
        for (size_t i = 0; i < n.kids.size(); ++i) n.key += (i ? "," : "") + n.kids[i]->key;
        n.key += ")";
    }
    return std::make_shared<const Node>(std::move(n));
}

Ex K(int64_t v) { return makeNode(Node{"k", v, "", {}, ""}); }
Ex atom(const std::string &s) { return makeNode(Node{"atom", 0, s, {}, ""}); }
Ex T() { return atom("#true"); }
Ex F() { return atom("#false"); }
bool isK(const Ex &e, int64_t v) { return e->op == "k" && e->k == v; }
bool isTrue(const Ex &e) { return e->key == "#true"; }
bool isFalse(const Ex &e) { return e->key == "#false"; }
bool same(const Ex &a, const Ex &b) { return a->key == b->key; }

bool isBool(const Ex &e) {
    static const std::set<std::string> ops{"eq", "ne", "lt", "le", "and", "or", "not"};
    if (isTrue(e) || isFalse(e) || ops.count(e->op)) return true;
    return e->op == "ite" && isBool(e->kids[1]) && isBool(e->kids[2]);
}

Ex mk(const std::string &op, std::vector<Ex> kids);

Ex notE(const Ex &a) { return mk("not", {a}); }

Ex mk(const std::string &op, std::vector<Ex> kids) {
    static const std::set<std::string> commutative{"add", "mul", "eq", "ne", "and", "or"};
    if (commutative.count(op) && kids.size() == 2 && kids[1]->key < kids[0]->key) std::swap(kids[0], kids[1]);
    auto both = [&]() { return kids.size() == 2 && kids[0]->op == "k" && kids[1]->op == "k"; };
    const Ex &a = kids[0];
    if (op == "add") {
        if (both()) return K(a->k + kids[1]->k);
        if (isK(a, 0)) return kids[1];
        if (isK(kids[1], 0)) return a;
    } else if (op == "sub") {
        if (both()) return K(a->k - kids[1]->k);
        if (isK(kids[1], 0)) return a;
    } else if (op == "mul") {
        if (both()) return K(a->k * kids[1]->k);
    } else if (op == "rem") {
        // (x + 1) % n is the increment; (x - 1 + n) % n the decrement.
        const Ex &x = a, &n = kids[1];
        if (x->op == "add" && isK(x->kids[0], 1)) return mk("inc", {x->kids[1], n});
        if (x->op == "add" && isK(x->kids[1], 1)) return mk("inc", {x->kids[0], n});
        if (x->op == "add")
            for (int i = 0; i < 2; ++i)
                if (same(x->kids[i], n) && x->kids[1 - i]->op == "sub" && isK(x->kids[1 - i]->kids[1], 1))
                    return mk("dec", {x->kids[1 - i]->kids[0], n});
    } else if (op == "eq" || op == "ne") {
        if (same(a, kids[1])) return op == "eq" ? T() : F();
        if (both()) return (a->k == kids[1]->k) == (op == "eq") ? T() : F();
        // h == (t - 1) mod n is (h + 1) mod n == t.
        for (int i = 0; i < 2; ++i)
            if (kids[i]->op == "dec") {
                Ex l = mk("inc", {kids[1 - i], kids[i]->kids[1]});
                return mk(op, {l, kids[i]->kids[0]});
            }
        // A truth value turned into an integer and back.
        for (int i = 0; i < 2; ++i)
            if (isK(kids[1 - i], 0) && kids[i]->op == "ite" && kids[i]->kids[1]->op == "k" &&
                isK(kids[i]->kids[2], 0) && !isK(kids[i]->kids[1], 0))
                return op == "ne" ? kids[i]->kids[0] : notE(kids[i]->kids[0]);
        if (op == "ne" && isBool(a) && isFalse(kids[1])) return a;
        if (op == "ne" && isBool(kids[1]) && isFalse(a)) return kids[1];
    } else if (op == "lt" || op == "le") {
        if (both()) return (op == "lt" ? a->k < kids[1]->k : a->k <= kids[1]->k) ? T() : F();
        if (op == "le" && same(a, kids[1])) return T();
        if (op == "lt" && same(a, kids[1])) return F();
    } else if (op == "not") {
        if (isTrue(a)) return F();
        if (isFalse(a)) return T();
        if (a->op == "not") return a->kids[0];
        if (a->op == "eq") return mk("ne", a->kids);
        if (a->op == "ne") return mk("eq", a->kids);
        if (a->op == "lt") return mk("le", {a->kids[1], a->kids[0]});
        if (a->op == "le") return mk("lt", {a->kids[1], a->kids[0]});
    } else if (op == "and") {
        if (isTrue(a)) return kids[1];
        if (isTrue(kids[1])) return a;
        if (isFalse(a) || isFalse(kids[1])) return F();
        if (same(a, kids[1])) return a;
    } else if (op == "or") {
        if (isFalse(a)) return kids[1];
        if (isFalse(kids[1])) return a;
        if (isTrue(a) || isTrue(kids[1])) return T();
        if (same(a, kids[1])) return a;
    } else if (op == "ite") {
        const Ex &c = a, &x = kids[1], &y = kids[2];
        if (isTrue(c)) return x;
        if (isFalse(c)) return y;
        if (same(x, y)) return x;
        if (c->op == "not") return mk("ite", {c->kids[0], y, x});
        if (isBool(x) && isBool(y)) {
            if (isTrue(x) && isFalse(y)) return c;
            if (isFalse(x) && isTrue(y)) return notE(c);
            if (isFalse(y)) return mk("and", {c, x});
            if (isTrue(x)) return mk("or", {c, y});
            if (isFalse(x)) return mk("and", {notE(c), y});
            if (isTrue(y)) return mk("or", {notE(c), x});
        }
        // if (x + 1 == n || x + 1 >= n) wrap to zero: the increment.
        if (isK(x, 0) && y->op == "add") {
            Ex inner;
            if (isK(y->kids[0], 1)) inner = y->kids[1];
            else if (isK(y->kids[1], 1)) inner = y->kids[0];
            if (inner) {
                if (c->op == "eq" && (same(c->kids[0], y) || same(c->kids[1], y)))
                    return mk("inc", {inner, same(c->kids[0], y) ? c->kids[1] : c->kids[0]});
                if (c->op == "le" && same(c->kids[1], y)) return mk("inc", {inner, c->kids[0]});
            }
        }
    }
    Node n;
    n.op = op;
    n.kids = std::move(kids);
    return makeNode(std::move(n));
}

// A boolean seen as an integer, and back (C's conditions are ints).
Ex toBool(const Ex &e) { return isBool(e) ? e : mk("ne", {e, K(0)}); }
Ex toInt(const Ex &e) { return isBool(e) ? mk("ite", {e, K(1), K(0)}) : e; }

// ---------------------------------------------------------------------------
// The evaluation of one function.

// Where a pointer points: a member path below a parameter of the function
// read (the struct, the caller's ring, an out parameter), a local, or an
// element of an array whose base and index are values.
struct Addr {
    enum Kind { None, Field, Slot, Elem } kind = None;
    std::string root;
    std::vector<unsigned> path;
    Operation *slot = nullptr;
    Ex base, idx;
    std::string fieldKey() const {
        std::string s = root + ":";
        for (size_t i = 0; i < path.size(); ++i) s += (i ? "." : "") + std::to_string(path[i]);
        return s;
    }
};

struct Val {
    Ex ex;
    Addr addr;
};

struct Event {
    std::string kind;  // lock unlock wait signal broadcast minit mdestroy cinit cdestroy malloc free eload estore transfer
    std::string a, b;  // the mutex's or the variable's member, as a field key
    Ex x, y, z;
    // transfer: the rings, as roots and member paths, and the limit
    struct Ring {
        std::string root;
        std::vector<unsigned> d, in, out, s;
    } src, dst;
    std::string key() const {
        std::string k = kind + "[" + a + "," + b;
        for (const Ex &e : {x, y, z}) k += "," + (e ? e->key : std::string());
        if (kind == "transfer") k += "," + src.root + "->" + dst.root;
        return k + "]";
    }
};

std::string pathStr(const std::vector<unsigned> &p) {
    std::string s;
    for (size_t i = 0; i < p.size(); ++i) s += (i ? "." : "") + std::to_string(p[i]);
    return s;
}

struct Lit {
    unsigned fork;  // 0: a fact, never a branch
    Ex e;
    bool pol;
};

struct Path {
    llvm::DenseMap<Value, Val> vals;
    std::map<std::string, Ex> fields;  // members written since the root's last havoc
    std::map<Operation *, Val> slots;
    std::vector<Lit> conds;
    std::vector<Event> events;
    std::map<std::string, unsigned> gen;
    bool locked = false, unlockedAccess = false, aborted = false;
    Ex ret, captured;
};

void mergePaths(std::vector<Path> &ps);
std::optional<unsigned> paramOf(const Ex &e);

const char *kNoReturn[] = {"__assert_fail", "__assert_rtn", "abort", "exit", "_exit"};

class Eval {
public:
    Eval(cir::FuncOp f, std::string qRoot, const std::set<std::string> *mutableKeys)
        : f(f), qRoot(std::move(qRoot)), mutableKeys(mutableKeys) {}

    std::string why;
    std::set<std::string> written;  // field keys written, without generations
    std::set<std::string> unknownCallees;
    // The branches on which the function ends the program (an assertion,
    // abort, exit): each one's conditions, which the substitute must make
    // as well or the pass refuses (docs/lifting.md, "C queues").
    std::vector<std::vector<Lit>> aborts;
    // Parameters converted to an integer type that cannot hold all of
    // their values (narrower, or of the other signedness).
    std::set<unsigned> lossyParams;

    // Every path through the function, or none with the reason in `why`.
    bool run(std::vector<Path> &out) {
        if (f.isDeclaration()) return fail("has no body here");
        Block &entry = f.getBody().front();
        Path p;
        for (BlockArgument a : entry.getArguments()) {
            Val v;
            const std::string r = "P" + std::to_string(a.getArgNumber());
            v.ex = atom(r);
            if (isa<cir::PointerType>(a.getType())) {
                v.addr.kind = Addr::Field;
                v.addr.root = r;
            }
            p.vals[a] = v;
        }
        done = &out;
        go(entry.begin(), &entry, std::move(p), nullptr, 0);
        return !failed;
    }

    // The atom naming member `key` as it is now, in this path.
    Ex member(const Path &p, const std::string &key, const std::string &root) const {
        if (mutableKeys && !mutableKeys->count(key)) return atom("f:" + key);
        auto g = p.gen.find(root);
        return atom("f" + std::to_string(g == p.gen.end() ? 0 : g->second) + ":" + key);
    }

private:
    struct Cont {
        enum Kind { Region, Call, Capture } kind;
        Block::iterator it;
        Block *block;
        const Cont *up;
        cir::CallOp call;
        std::vector<Path> *capture = nullptr;
    };

    cir::FuncOp f;
    std::string qRoot;
    const std::set<std::string> *mutableKeys;
    std::vector<Path> *done = nullptr;
    bool failed = false;
    bool inWaitBody = false;
    unsigned forks = 0;

    bool fail(const Twine &w) {
        if (why.empty()) why = w.str();
        failed = true;
        return false;
    }

    std::optional<Val> val(Path &p, Value v) {
        auto it = p.vals.find(v);
        if (it != p.vals.end()) return it->second;
        if (auto a = v.getDefiningOp<cir::AllocaOp>()) {
            Val s;
            s.ex = atom("slot");
            s.addr.kind = Addr::Slot;
            s.addr.slot = a;
            return s;
        }
        fail("reads a value the pass cannot follow");
        return std::nullopt;
    }

    void touch(Path &p, const Addr &a) {
        if (a.kind == Addr::Field && a.root == qRoot && !p.locked) p.unlockedAccess = true;
    }

    // Literals the path already holds decide a branch.
    std::optional<bool> decided(const Path &p, const Ex &c) {
        if (isTrue(c)) return true;
        if (isFalse(c)) return false;
        const Ex n = notE(c);
        for (const Lit &l : p.conds) {
            if (same(l.e, c)) return l.pol;
            if (same(l.e, n)) return !l.pol;
        }
        return std::nullopt;
    }

    // The value a region yields, read without effects: its paths' values,
    // merged over the branches inside it; a branch whose other side ends
    // the program (an assertion) is assumed.
    std::optional<Ex> pure(Region &r, const Path &p, unsigned depth) {
        if (r.empty()) return std::nullopt;
        std::vector<Path> got;
        Cont cap{Cont::Capture, {}, nullptr, nullptr, {}, &got};
        go(r.front().begin(), &r.front(), p, &cap, depth + 1);
        if (failed) return std::nullopt;
        std::vector<std::pair<std::vector<Lit>, Ex>> vs;
        for (Path &q : got) {
            if (q.aborted) continue;
            if (q.events.size() != p.events.size() || q.fields != p.fields) {
                fail("does something in a condition");
                return std::nullopt;
            }
            if (!q.captured) {
                fail("has a condition the pass cannot read");
                return std::nullopt;
            }
            std::vector<Lit> mine(q.conds.begin() + p.conds.size(), q.conds.end());
            vs.push_back({mine, q.captured});
        }
        if (vs.empty()) {
            fail("has a condition that always ends the program");
            return std::nullopt;
        }
        // Merge pairs that differ in one branch.
        bool changed = true;
        while (changed && vs.size() > 1) {
            changed = false;
            for (size_t i = 0; i < vs.size() && !changed; ++i)
                for (size_t j = i + 1; j < vs.size() && !changed; ++j) {
                    auto &[ci, vi] = vs[i];
                    auto &[cj, vj] = vs[j];
                    if (ci.size() != cj.size()) continue;
                    int diff = -1;
                    bool ok = true;
                    for (size_t x = 0; x < ci.size() && ok; ++x) {
                        if (ci[x].fork == cj[x].fork && same(ci[x].e, cj[x].e) && ci[x].pol == cj[x].pol) continue;
                        if (diff < 0 && ci[x].fork == cj[x].fork && ci[x].fork && same(ci[x].e, cj[x].e))
                            diff = static_cast<int>(x);
                        else ok = false;
                    }
                    if (!ok || diff < 0) continue;
                    const Lit l = ci[diff];
                    Ex v = l.pol ? mk("ite", {l.e, vi, vj}) : mk("ite", {l.e, vj, vi});
                    std::vector<Lit> c = ci;
                    c.erase(c.begin() + diff);
                    vs[i] = {c, v};
                    vs.erase(vs.begin() + j);
                    changed = true;
                }
        }
        if (vs.size() != 1) {
            fail("has a condition with several outcomes the pass cannot combine");
            return std::nullopt;
        }
        return vs[0].second;
    }

    // Continues after the end of a region: at the region's operation's
    // successor, or out of a call, or into a capture.
    void leave(Path p, const Cont *k, Value yielded, unsigned depth) {
        if (!k) return (void)fail("leaves a region the pass did not enter");
        if (k->kind == Cont::Capture) {
            if (yielded) {
                auto v = val(p, yielded);
                if (!v) return;
                p.captured = v->ex;
            }
            k->capture->push_back(std::move(p));
            return;
        }
        if (k->kind == Cont::Call) return (void)fail("leaves a function other than by return");
        go(k->it, k->block, std::move(p), k->up, depth + 1);
    }

    void finish(Path p) { done->push_back(std::move(p)); }

    void go(Block::iterator it, Block *b, Path p, const Cont *k, unsigned depth);
    void call(cir::CallOp c, Block::iterator next, Block *b, Path p, const Cont *k, unsigned depth);
    void waitLoop(cir::WhileOp w, Block::iterator next, Block *b, Path p, const Cont *k, unsigned depth);
    void transferLoop(cir::ForOp l, Block::iterator next, Block *b, Path p, const Cont *k, unsigned depth);
};

const char *cmpOp(cir::CmpOpKind k, bool &swap) {
    swap = false;
    switch (k) {
    case cir::CmpOpKind::eq: return "eq";
    case cir::CmpOpKind::ne: return "ne";
    case cir::CmpOpKind::lt: return "lt";
    case cir::CmpOpKind::le: return "le";
    case cir::CmpOpKind::gt: swap = true; return "lt";
    case cir::CmpOpKind::ge: swap = true; return "le";
    default: return nullptr;
    }
}

void Eval::go(Block::iterator it, Block *b, Path p, const Cont *k, unsigned depth) {
    if (failed) return;
    if (depth > 200) return (void)fail("is too deep to read");
    for (; it != b->end(); ++it) {
        Operation *op = &*it;
        if (isa<cir::AllocaOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(op)) continue;
        if (auto c = dyn_cast<cir::ConstantOp>(op)) {
            Val v;
            if (auto i = dyn_cast<cir::IntAttr>(c.getValue())) v.ex = K(i.getValue().getSExtValue());
            else if (auto bo = dyn_cast<cir::BoolAttr>(c.getValue())) v.ex = bo.getValue() ? T() : F();
            else if (auto cp = dyn_cast<cir::ConstPtrAttr>(c.getValue()); cp && cp.isNullValue()) v.ex = atom("null");
            else return (void)fail("uses a constant the pass cannot read");
            p.vals[c] = v;
            continue;
        }
        if (auto g = dyn_cast<cir::GetGlobalOp>(op)) {
            p.vals[g] = Val{atom("G:" + g.getName().str()), {}};
            continue;
        }
        if (auto gm = dyn_cast<cir::GetMemberOp>(op)) {
            auto base = val(p, gm.getAddr());
            if (!base) return;
            if (base->addr.kind != Addr::Field) return (void)fail("reaches a member through a pointer it loaded");
            Val v;
            v.addr = base->addr;
            v.addr.path.push_back(gm.getIndex());
            v.ex = atom("&" + v.addr.fieldKey());
            p.vals[gm] = v;
            continue;
        }
        if (auto ps = dyn_cast<cir::PtrStrideOp>(op)) {
            auto base = val(p, ps.getBase());
            auto idx = val(p, ps.getStride());
            if (!base || !idx) return;
            Val v;
            v.addr.kind = Addr::Elem;
            v.addr.base = base->ex;
            v.addr.idx = idx->ex;
            v.ex = mk("elem", {base->ex, idx->ex});
            p.vals[ps] = v;
            continue;
        }
        if (auto ld = dyn_cast<cir::LoadOp>(op)) {
            auto a = val(p, ld.getAddr());
            if (!a) return;
            Val v;
            switch (a->addr.kind) {
            case Addr::Slot: {
                auto s = p.slots.find(a->addr.slot);
                if (s == p.slots.end()) return (void)fail("reads a local before writing it");
                v = s->second;
                break;
            }
            case Addr::Field: {
                touch(p, a->addr);
                const std::string key = a->addr.fieldKey();
                auto fi = p.fields.find(key);
                v.ex = fi != p.fields.end() ? fi->second : member(p, key, a->addr.root);
                break;
            }
            case Addr::Elem: {
                v.ex = mk("eload", {a->addr.base, a->addr.idx});
                Event e;
                e.kind = "eload";
                e.x = a->addr.base;
                e.y = a->addr.idx;
                p.events.push_back(e);
                break;
            }
            case Addr::None:
                return (void)fail("reads memory the pass cannot place");
            }
            p.vals[ld] = v;
            continue;
        }
        if (auto st = dyn_cast<cir::StoreOp>(op)) {
            auto v = val(p, st.getValue());
            auto a = val(p, st.getAddr());
            if (!v || !a) return;
            switch (a->addr.kind) {
            case Addr::Slot: p.slots[a->addr.slot] = *v; break;
            case Addr::Field: {
                touch(p, a->addr);
                const std::string key = a->addr.fieldKey();
                p.fields[key] = v->ex;
                written.insert(key);
                break;
            }
            case Addr::Elem: {
                Event e;
                e.kind = "estore";
                e.x = a->addr.base;
                e.y = a->addr.idx;
                e.z = v->ex;
                p.events.push_back(e);
                break;
            }
            case Addr::None:
                return (void)fail("writes memory the pass cannot place");
            }
            continue;
        }
        if (isa<cir::AddOp, cir::SubOp, cir::MulOp, cir::RemOp>(op)) {
            auto l = val(p, op->getOperand(0)), r = val(p, op->getOperand(1));
            if (!l || !r) return;
            const char *o = isa<cir::AddOp>(op) ? "add" : isa<cir::SubOp>(op) ? "sub" : isa<cir::MulOp>(op) ? "mul" : "rem";
            p.vals[op->getResult(0)] = Val{mk(o, {l->ex, r->ex}), {}};
            continue;
        }
        if (auto c = dyn_cast<cir::CmpOp>(op)) {
            auto l = val(p, c.getLhs()), r = val(p, c.getRhs());
            if (!l || !r) return;
            bool swap = false;
            const char *o = cmpOp(c.getKind(), swap);
            if (!o) return (void)fail("compares in a way the pass cannot read");
            p.vals[c] = Val{swap ? mk(o, {r->ex, l->ex}) : mk(o, {l->ex, r->ex}), {}};
            continue;
        }
        if (isa<cir::IncOp, cir::DecOp>(op)) {
            auto x = val(p, op->getOperand(0));
            if (!x) return;
            p.vals[op->getResult(0)] = Val{mk(isa<cir::IncOp>(op) ? "add" : "sub", {x->ex, K(1)}), {}};
            continue;
        }
        if (auto n = dyn_cast<cir::NotOp>(op)) {
            auto x = val(p, n.getInput());
            if (!x) return;
            if (!isa<cir::BoolType>(n.getInput().getType())) return (void)fail("complements bits");
            p.vals[n] = Val{notE(x->ex), {}};
            continue;
        }
        if (auto c = dyn_cast<cir::CastOp>(op)) {
            auto x = val(p, c.getSrc());
            if (!x) return;
            Val v = *x;
            switch (c.getKind()) {
            case cir::CastKind::integral: {
                v.addr = {};
                auto from = dyn_cast<cir::IntType>(c.getSrc().getType());
                auto to = dyn_cast<cir::IntType>(c.getType());
                const bool keeps = from && to &&
                                   ((from.getWidth() == to.getWidth() && from.isSigned() == to.isSigned()) ||
                                    (from.getWidth() < to.getWidth() && (from.isUnsigned() || to.isSigned())));
                if (!keeps)
                    if (auto pp = paramOf(x->ex)) lossyParams.insert(*pp);
                break;
            }
            case cir::CastKind::int_to_bool: v = Val{toBool(x->ex), {}}; break;
            case cir::CastKind::bool_to_int: v = Val{toInt(x->ex), {}}; break;
            case cir::CastKind::ptr_to_bool: v = Val{mk("ne", {x->ex, atom("null")}), {}}; break;
            case cir::CastKind::bitcast:
            case cir::CastKind::array_to_ptrdecay: break;
            default: return (void)fail("converts a value in a way the pass cannot read");
            }
            p.vals[c] = v;
            continue;
        }
        if (auto s = dyn_cast<cir::SelectOp>(op)) {
            auto c = val(p, s.getCondition()), x = val(p, s.getTrueValue()), y = val(p, s.getFalseValue());
            if (!c || !x || !y) return;
            p.vals[s] = Val{mk("ite", {toBool(c->ex), x->ex, y->ex}), {}};
            continue;
        }
        if (auto t = dyn_cast<cir::TernaryOp>(op)) {
            auto c = val(p, t.getCond());
            if (!c) return;
            auto known = decided(p, toBool(c->ex));
            std::optional<Ex> x, y;
            if (!known || *known) x = pure(t.getTrueRegion(), p, depth);
            if (failed) return;
            if (!known || !*known) y = pure(t.getFalseRegion(), p, depth);
            if (failed) return;
            p.vals[t.getResult()] = Val{known ? (*known ? *x : *y) : mk("ite", {toBool(c->ex), *x, *y}), {}};
            continue;
        }
        if (auto call = dyn_cast<cir::CallOp>(op)) return this->call(call, std::next(it), b, std::move(p), k, depth);
        if (auto sc = dyn_cast<cir::ScopeOp>(op)) {
            if (sc.getScopeRegion().empty()) continue;
            Cont c{Cont::Region, std::next(it), b, k, {}, nullptr};
            Block &inner = sc.getScopeRegion().front();
            return go(inner.begin(), &inner, std::move(p), &c, depth + 1);
        }
        if (auto cl = dyn_cast<cir::CleanupScopeOp>(op)) {
            // A C scope's cleanup ends its locals' lifetimes and does nothing else.
            Region &cr = cl.getCleanupRegion();
            if (!cr.empty())
                for (Operation &o : cr.front())
                    if (!isa<cir::YieldOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(o))
                        return (void)fail("has a cleanup that does more than end its locals' lifetimes");
            Cont c{Cont::Region, std::next(it), b, k, {}, nullptr};
            Block &inner = cl.getBodyRegion().front();
            return go(inner.begin(), &inner, std::move(p), &c, depth + 1);
        }
        if (auto i = dyn_cast<cir::IfOp>(op)) {
            auto c0 = val(p, i.getCondition());
            if (!c0) return;
            const Ex c = toBool(c0->ex);
            Cont fr{Cont::Region, std::next(it), b, k, {}, nullptr};
            auto branch = [&](Region &r, Path q) {
                if (r.empty()) return go(fr.it, fr.block, std::move(q), fr.up, depth + 1);
                Block &inner = r.front();
                go(inner.begin(), &inner, std::move(q), &fr, depth + 1);
            };
            if (auto d = decided(p, c)) return branch(*d ? i.getThenRegion() : i.getElseRegion(), std::move(p));
            const unsigned id = ++forks;
            Path q = p;
            q.conds.push_back({id, c, false});
            p.conds.push_back({id, c, true});
            branch(i.getThenRegion(), std::move(p));
            if (failed) return;
            return branch(i.getElseRegion(), std::move(q));
        }
        if (auto w = dyn_cast<cir::WhileOp>(op)) return waitLoop(w, std::next(it), b, std::move(p), k, depth);
        if (auto l = dyn_cast<cir::ForOp>(op)) return transferLoop(l, std::next(it), b, std::move(p), k, depth);
        if (isa<cir::DoWhileOp>(op)) return (void)fail("has a do-while loop");
        if (auto y = dyn_cast<cir::YieldOp>(op))
            return leave(std::move(p), k, y.getNumOperands() ? y.getOperand(0) : Value(), depth);
        if (auto c = dyn_cast<cir::ConditionOp>(op)) return leave(std::move(p), k, c.getCondition(), depth);
        if (auto r = dyn_cast<cir::ReturnOp>(op)) {
            Ex rv;
            if (r.getNumOperands() == 1) {
                auto v = val(p, r.getOperand(0));
                if (!v) return;
                rv = v->ex;
            }
            // Out of the innermost call, if any; no capture may lie between.
            for (const Cont *c = k; c; c = c->up) {
                if (c->kind == Cont::Capture) return (void)fail("returns from inside a condition or a loop");
                if (c->kind == Cont::Call) {
                    cir::CallOp site = c->call;
                    if (site.getNumResults() == 1) {
                        if (!rv) return (void)fail("returns nothing where a value is used");
                        p.vals[site.getResult()] = Val{rv, {}};
                    }
                    return go(c->it, c->block, std::move(p), c->up, depth + 1);
                }
            }
            p.ret = rv;
            return finish(std::move(p));
        }
        return (void)fail("does something the pass cannot read (" + op->getName().getStringRef() + ")");
    }
    leave(std::move(p), k, Value(), depth);
}

void Eval::call(cir::CallOp c, Block::iterator next, Block *b, Path p, const Cont *k, unsigned depth) {
    cir::FuncOp callee = lookupCallee(c);
    if (!callee) return (void)fail("makes an indirect call");
    const StringRef name = callee.getSymName();
    std::vector<Val> args;
    for (Value a : c.getArgOperands()) {
        auto v = val(p, a);
        if (!v) return;
        args.push_back(*v);
    }
    auto result = [&](Ex e) {
        if (c.getNumResults() == 1) p.vals[c.getResult()] = Val{e, {}};
    };
    auto fieldArg = [&](unsigned i) -> std::optional<std::string> {
        if (i >= args.size() || args[i].addr.kind != Addr::Field) return std::nullopt;
        return args[i].addr.fieldKey();
    };
    auto event = [&](const char *kind, std::optional<std::string> a, std::optional<std::string> bb = {}) {
        Event e;
        e.kind = kind;
        e.a = a.value_or("");
        e.b = bb.value_or("");
        p.events.push_back(e);
    };
    for (const char *nr : kNoReturn)
        if (name == nr) {
            aborts.push_back(p.conds);
            p.aborted = true;
            return finish(std::move(p));
        }
    if (name == "pthread_mutex_lock" || name == "pthread_mutex_unlock") {
        auto m = fieldArg(0);
        if (!m) return (void)fail("locks a mutex the pass cannot place");
        const bool lock = name == "pthread_mutex_lock";
        if (lock == p.locked) return (void)fail(lock ? "locks the mutex twice" : "unlocks a mutex it does not hold");
        p.locked = lock;
        event(lock ? "lock" : "unlock", m);
        result(K(0));
        return go(next, b, std::move(p), k, depth + 1);
    }
    if (name == "pthread_cond_wait") {
        if (!inWaitBody) return (void)fail("waits outside a loop that tests what it waits for");
        auto cv = fieldArg(0), m = fieldArg(1);
        if (!cv || !m) return (void)fail("waits on a variable the pass cannot place");
        event("wait", cv, m);
        result(K(0));
        return go(next, b, std::move(p), k, depth + 1);
    }
    if (name == "pthread_cond_timedwait") return (void)fail("waits with a time limit");
    const std::map<StringRef, const char *> simple{
        {"pthread_cond_signal", "signal"}, {"pthread_cond_broadcast", "broadcast"},
        {"pthread_mutex_init", "minit"},   {"pthread_mutex_destroy", "mdestroy"},
        {"pthread_cond_init", "cinit"},    {"pthread_cond_destroy", "cdestroy"}};
    if (auto s = simple.find(name); s != simple.end()) {
        auto a = fieldArg(0);
        if (!a) return (void)fail(Twine("calls ") + name + " on an object the pass cannot place");
        event(s->second, a);
        result(K(0));
        return go(next, b, std::move(p), k, depth + 1);
    }
    if (name == "malloc" && args.size() == 1) {
        Event e;
        e.kind = "malloc";
        e.x = args[0].ex;
        p.events.push_back(e);
        result(mk("malloc", {args[0].ex}));
        return go(next, b, std::move(p), k, depth + 1);
    }
    if (name == "free" && args.size() == 1) {
        Event e;
        e.kind = "free";
        e.x = args[0].ex;
        p.events.push_back(e);
        return go(next, b, std::move(p), k, depth + 1);
    }
    if (callee.isDeclaration()) return (void)fail(Twine("calls ") + name + ", which the pass cannot read");
    for (const Cont *x = k; x; x = x->up)
        if (x->kind == Cont::Call && lookupCallee(cir::CallOp(x->call)) == callee) return (void)fail("is recursive");
    Block &entry = callee.getBody().front();
    if (entry.getNumArguments() != args.size()) return (void)fail(Twine("calls ") + name + " with a varying number of arguments");
    for (auto [a, v] : llvm::zip(entry.getArguments(), args)) p.vals[a] = v;
    Cont ret{Cont::Call, next, b, k, c, nullptr};
    go(entry.begin(), &entry, std::move(p), &ret, depth + 1);
}

// while (pred) pthread_cond_wait(&cv, &m): the wait ends when pred is
// false, in a state other threads may have changed.
void Eval::waitLoop(cir::WhileOp w, Block::iterator next, Block *b, Path p, const Cont *k, unsigned depth) {
    auto pred0 = pure(w.getCond(), p, depth);
    if (!pred0) return;
    std::vector<Path> got;
    Cont cap{Cont::Capture, {}, nullptr, nullptr, {}, &got};
    inWaitBody = true;
    go(w.getBody().front().begin(), &w.getBody().front(), p, &cap, depth + 1);
    inWaitBody = false;
    if (failed) return;
    if (got.size() != 1 || got[0].aborted || got[0].events.size() != p.events.size() + 1 ||
        got[0].events.back().kind != "wait" || got[0].fields != p.fields)
        return (void)fail("has a loop other than one that waits on a condition variable");
    Event e = got[0].events.back();
    if (!p.locked) return (void)fail("waits without the mutex");
    e.x = toBool(*pred0);
    p.events.push_back(e);
    // Another thread may have changed the struct.
    p.gen[qRoot] = p.gen[qRoot] + 1;
    for (auto it = p.fields.begin(); it != p.fields.end();)
        it = StringRef(it->first).starts_with(qRoot + ":") ? p.fields.erase(it) : std::next(it);
    auto pred1 = pure(w.getCond(), p, depth);
    if (!pred1) return;
    p.conds.push_back({0, toBool(*pred1), false});
    go(next, b, std::move(p), k, depth + 1);
}

// for (i = 0; i < limit && !empty(src) && !full(dst); i++) insert(dst,
// remove(src)): a run of moves from one ring to the other.
void Eval::transferLoop(cir::ForOp l, Block::iterator next, Block *b, Path p, const Cont *k, unsigned depth) {
    // The step: i = i + 1, and nothing else.
    Operation *slot = nullptr;
    {
        Block &s = l.getStep().front();
        llvm::SmallVector<Operation *, 4> ops;
        for (Operation &o : s) ops.push_back(&o);
        auto ld = ops.size() == 4 ? dyn_cast<cir::LoadOp>(ops[0]) : cir::LoadOp();
        auto inc = ld ? dyn_cast<cir::IncOp>(ops[1]) : cir::IncOp();
        auto st = inc ? dyn_cast<cir::StoreOp>(ops[2]) : cir::StoreOp();
        if (!st || inc.getInput() != ld.getResult() || st.getValue() != inc.getResult() ||
            st.getAddr() != ld.getAddr() || !ld.getAddr().getDefiningOp<cir::AllocaOp>())
            return (void)fail("has a loop whose step is not a count");
        slot = ld.getAddr().getDefiningOp();
    }
    auto i0 = p.slots.find(slot);
    if (i0 == p.slots.end() || !isK(i0->second.ex, 0)) return (void)fail("has a loop whose count does not start at zero");
    // One iteration from any state: every pointer parameter's members are
    // what they are by then.
    Path q = p;
    std::set<std::string> roots;
    for (BlockArgument a : f.getBody().front().getArguments())
        if (isa<cir::PointerType>(a.getType())) roots.insert("P" + std::to_string(a.getArgNumber()));
    for (const std::string &r : roots) q.gen[r] = q.gen[r] + 1;
    q.fields.clear();
    q.slots[slot] = Val{atom("i"), {}};
    auto cond = pure(l.getCond(), q, depth);
    if (!cond) return;
    std::vector<Ex> conj;
    std::function<void(const Ex &)> flat = [&](const Ex &e) {
        if (e->op == "and") {
            flat(e->kids[0]);
            flat(e->kids[1]);
        } else {
            conj.push_back(e);
        }
    };
    flat(toBool(*cond));
    for (const Ex &c : conj) q.conds.push_back({0, c, true});
    std::vector<Path> got;
    Cont cap{Cont::Capture, {}, nullptr, nullptr, {}, &got};
    go(l.getBody().front().begin(), &l.getBody().front(), q, &cap, depth + 1);
    if (failed) return;
    mergePaths(got);
    if (got.size() != 1) return (void)fail("has a loop whose body the pass cannot read as one move");
    Path &it1 = got[0];
    // The body: one element read from one ring and stored into the other,
    // each ring's index advanced.
    std::vector<Event> ev(it1.events.begin() + q.events.size(), it1.events.end());
    if (ev.size() != 2 || ev[0].kind != "eload" || ev[1].kind != "estore" ||
        !same(ev[1].z, mk("eload", {ev[0].x, ev[0].y})))
        return (void)fail("has a loop that does more than move one element between two rings");
    if (it1.fields.size() != 2) return (void)fail("has a loop that changes more than the two rings' indices");
    auto parse = [](const Ex &e, std::string &root, std::vector<unsigned> &path) {
        if (e->op != "atom" || e->name.empty() || e->name[0] != 'f') return false;
        StringRef s(e->name);
        s = s.drop_until([](char c) { return c == ':'; }).drop_front();
        auto [r, rest] = s.split(':');
        root = r.str();
        path.clear();
        if (rest.empty()) return true;
        llvm::SmallVector<StringRef, 4> parts;
        rest.split(parts, '.');
        for (StringRef x : parts) {
            unsigned v;
            if (x.getAsInteger(10, v)) return false;
            path.push_back(v);
        }
        return true;
    };
    Event t;
    t.kind = "transfer";
    std::vector<unsigned> tmp;
    if (!parse(ev[0].x, t.src.root, t.src.d) || !parse(ev[0].y, t.src.root, t.src.out) ||
        !parse(ev[1].x, t.dst.root, t.dst.d) || !parse(ev[1].y, t.dst.root, t.dst.in))
        return (void)fail("has a loop that moves elements the pass cannot place");
    std::string r1, r2;
    // Each ring's index: advanced by one modulo its slot count.
    for (auto &[ring, idx, ex] : {std::tuple{&t.src, &t.src.out, ev[0].y}, std::tuple{&t.dst, &t.dst.in, ev[1].y}}) {
        const std::string key = ring->root + ":" + pathStr(*idx);
        auto w = it1.fields.find(key);
        if (w == it1.fields.end() || w->second->op != "inc" || !same(w->second->kids[0], ex))
            return (void)fail("has a loop that does not advance a ring's index by one");
        std::string sr;
        if (!parse(w->second->kids[1], sr, ring->s) || sr != ring->root)
            return (void)fail("has a loop that wraps an index at something other than its ring's size");
    }
    if (t.src.root == t.dst.root) return (void)fail("has a loop that moves elements within one ring");
    // The condition: the count below a limit, the source not empty, the
    // destination not full; which names each ring's other index.
    Ex limit;
    bool srcTested = false, dstTested = false;
    const Ex srcOut = member(q, t.src.root + ":" + pathStr(t.src.out), t.src.root);
    const Ex dstIn = member(q, t.dst.root + ":" + pathStr(t.dst.in), t.dst.root);
    for (const Ex &c : conj) {
        if (c->op == "lt" && c->kids[0]->key == "i" && !limit) {
            limit = c->kids[1];
            continue;
        }
        bool matched = false;
        if (c->op == "ne") {
            for (int s = 0; s < 2 && !matched; ++s) {
                const Ex &x = c->kids[s], &y = c->kids[1 - s];
                std::string root, sr;
                std::vector<unsigned> path, sp;
                if (!srcTested && same(x, srcOut) && parse(y, root, path) && root == t.src.root) {
                    t.src.in = path;
                    srcTested = matched = true;
                } else if (!dstTested && x->op == "inc" && same(x->kids[0], dstIn) && parse(y, root, path) &&
                           root == t.dst.root && parse(x->kids[1], sr, sp) && sr == t.dst.root && sp == t.dst.s) {
                    t.dst.out = path;
                    dstTested = matched = true;
                }
            }
        }
        if (!matched) return (void)fail("has a loop whose condition tests " + c->key);
    }
    if (!limit || !srcTested || !dstTested)
        return (void)fail("has a loop that does not stop at its limit, at an empty source and at a full destination");
    if (!limit->name.empty() && limit->name[0] == 'f')
        return (void)fail("has a loop whose limit is a member, not a parameter");
    t.x = limit;
    // A ring written before the loop on this path would lose the write
    // with the generation below: refused rather than followed.
    for (const std::string &r : {t.src.root, t.dst.root})
        for (auto &[key, v] : p.fields)
            if (StringRef(key).starts_with(r + ":"))
                return (void)fail("writes a ring before the loop that moves elements between them");
    // After the loop: the rings are what the moves left, the count is
    // how many moved.
    p.events.push_back(t);
    for (const std::string &r : {t.src.root, t.dst.root}) {
        p.gen[r] = p.gen[r] + 1;
        for (auto it = p.fields.begin(); it != p.fields.end();)
            it = StringRef(it->first).starts_with(r + ":") ? p.fields.erase(it) : std::next(it);
    }
    p.slots[slot] = Val{atom("moved"), {}};
    go(next, b, std::move(p), k, depth + 1);
}

// Merges paths that differ only in one branch's direction and in values,
// and drops the branches assumed (their other side always ends the
// program).
void mergePaths(std::vector<Path> &ps) {
    // Assumptions first: a branch none of whose other side survives.
    std::set<std::pair<unsigned, bool>> live;
    for (const Path &p : ps)
        if (!p.aborted)
            for (const Lit &l : p.conds) live.insert({l.fork, l.pol});
    for (Path &p : ps) {
        if (p.aborted) continue;
        std::vector<Lit> keep;
        for (const Lit &l : p.conds)
            if (!l.fork || live.count({l.fork, !l.pol})) keep.push_back(l);
        p.conds = keep;
    }
    ps.erase(std::remove_if(ps.begin(), ps.end(), [](const Path &p) { return p.aborted; }), ps.end());
    auto eventsKey = [](const Path &p) {
        std::string s;
        for (const Event &e : p.events) s += e.key() + ";";
        return s;
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < ps.size() && !changed; ++i)
            for (size_t j = i + 1; j < ps.size() && !changed; ++j) {
                Path &a = ps[i], &b = ps[j];
                if (a.conds.size() != b.conds.size() || eventsKey(a) != eventsKey(b) ||
                    a.unlockedAccess != b.unlockedAccess || a.gen != b.gen)
                    continue;
                int diff = -1;
                bool ok = true;
                for (size_t x = 0; x < a.conds.size() && ok; ++x) {
                    const Lit &la = a.conds[x], &lb = b.conds[x];
                    if (la.fork == lb.fork && same(la.e, lb.e) && la.pol == lb.pol) continue;
                    if (diff < 0 && la.fork && la.fork == lb.fork && same(la.e, lb.e)) diff = static_cast<int>(x);
                    else ok = false;
                }
                if (!ok || diff < 0) continue;
                const Lit l = a.conds[diff];
                Path &pos = l.pol ? a : b;
                Path &neg = l.pol ? b : a;
                Path m = pos;
                m.conds.erase(m.conds.begin() + diff);
                std::set<std::string> keys;
                for (auto &[key, v] : pos.fields) keys.insert(key);
                for (auto &[key, v] : neg.fields) keys.insert(key);
                bool okFields = true;
                for (const std::string &key : keys) {
                    auto x = pos.fields.find(key), y = neg.fields.find(key);
                    if (x == pos.fields.end() || y == neg.fields.end()) {
                        okFields = false;  // written on one side only: its old value is not at hand
                        break;
                    }
                    m.fields[key] = mk("ite", {l.e, x->second, y->second});
                }
                if (!okFields) {
                    // The member keeps its value on the other side.
                    continue;
                }
                if (pos.ret && neg.ret) m.ret = mk("ite", {l.e, pos.ret, neg.ret});
                else if (pos.ret || neg.ret) continue;
                ps[i] = m;
                ps.erase(ps.begin() + j);
                changed = true;
            }
    }
}

// ---------------------------------------------------------------------------
// The pass.

struct Ring {
    std::vector<unsigned> d, in, out, s;
    bool known = false;
    bool operator==(const Ring &o) const { return d == o.d && in == o.in && out == o.out && s == o.s; }
};

enum class Role { None, Helper, Init, Destroy, Push, Pop, Terminate, PushBatch, PopBatch };

const char *roleName(Role r) {
    switch (r) {
    case Role::Helper: return "helper";
    case Role::Init: return "init";
    case Role::Destroy: return "destroy";
    case Role::Push: return "push";
    case Role::Pop: return "pop";
    case Role::Terminate: return "terminate";
    case Role::PushBatch: return "batched push";
    case Role::PopBatch: return "batched pop";
    case Role::None: break;
    }
    return "none";
}

struct Fn {
    cir::FuncOp f;
    unsigned q = 0;
    std::string root;
    Role role = Role::None;
    std::vector<Path> paths;
    std::set<std::string> written;
    // What the rewrite needs.
    unsigned valueParam = 0, outParam = 0, ringParam = 0, limitParam = 0;
    Ex takeRet, closedRet;  // the pop's constants
    std::optional<int64_t> pushRet;
    Event::Ring ring;       // a batch's caller's ring
    std::vector<std::vector<unsigned>> freed;  // destroy: what it frees
    std::vector<cir::CallOp> calls;            // init: every call of it
    std::optional<int64_t> constRet;           // init, destroy, terminate: the constant returned
    std::vector<std::vector<Lit>> aborts;      // where it ends the program
    std::set<unsigned> lossy;                  // parameters converted with a loss
};

struct Candidate {
    cir::RecordType rec;
    std::string label;
    std::vector<Fn> fns;
    Ring ring;
    std::vector<unsigned> mutex, ended, producers, notEmpty, notFull;
    bool counted = false;
    // init: capacity = param + offset, producers = param
    unsigned capParam = 0, prodParam = 0;
    int64_t capOffset = 0;
    bool hasProducers = false;
    // Whether a test of the end count compares it for equality with the
    // number of producers (ferret's `end_count == prod_threads`), set by
    // Facts as it reads the tests. Without one, every test is `<` or `>=`,
    // under which terminations beyond the producers change nothing, and
    // the runtime ignores them as the original does; with one, the
    // original goes wrong after them (it waits for ever or takes from an
    // empty ring), and the runtime ends the program instead.
    mutable bool endEq = false;
    std::string why;
};

std::string fieldName(cir::RecordType rec, const std::vector<unsigned> &path,
                      const llvm::DenseMap<std::pair<Type, unsigned>, std::string> &names) {
    std::string s;
    Type t = rec;
    for (unsigned i : path) {
        auto r = dyn_cast<cir::RecordType>(t);
        if (!r) return s + ".?";
        auto it = names.find({r, i});
        s += (s.empty() ? "" : ".") + (it != names.end() ? it->second : std::to_string(i));
        t = r.getMembers()[i];
    }
    return s;
}

class LiftCQueuesPass : public PassWrapper<LiftCQueuesPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(LiftCQueuesPass)

    LiftCQueuesPass() = default;
    LiftCQueuesPass(const LiftCQueuesPass &other) : PassWrapper(other) {}

    StringRef getArgument() const final { return "wtc-lift-c-queues"; }
    StringRef getDescription() const final {
        return "Recognise C structs that keep a ring of pointers under a pthread mutex as queues, prove the "
               "substitution legal, and replace their functions with the runtime of shim/c_queue.h";
    }
    void getDependentDialects(DialectRegistry &registry) const override { registry.insert<cir::CIRDialect>(); }

    Option<bool> rewrite{*this, "rewrite", llvm::cl::init(true),
                         llvm::cl::desc("Replace the functions of every struct that lifts. With false the pass "
                                        "only reports.")};
    Option<std::string> namedElsewhere{
        *this, "named-elsewhere", llvm::cl::init(""),
        llvm::cl::desc("A file of the symbols the other files linked with this one define or reference")};
    Option<bool> openWorld{*this, "open-world", llvm::cl::init(false),
                           llvm::cl::desc("Other units may name any external symbol of this one")};

    void runOnOperation() override;

private:
    ModuleOp module;
    llvm::DenseMap<std::pair<Type, unsigned>, std::string> names;
    std::set<std::string> elsewhere;

    void analyse(Candidate &c);
    bool classify(Candidate &c, Fn &fn);
    bool confined(Candidate &c);
    bool localRing(Candidate &c, Fn &fn);
    void lift(Candidate &c);
    cir::FuncOp runtime(StringRef name, llvm::ArrayRef<Type> args, Type ret);
    cir::FuncOp thunk(Candidate &c, cir::FuncOp rt, llvm::ArrayRef<StringRef> ann);
    std::string field(Candidate &c, const std::vector<unsigned> &p) { return fieldName(c.rec, p, names); }
    std::string readable(Candidate &c, const Ex &e, const std::string &root);
    bool refuseWith(Candidate &c, const Twine &w) {
        if (c.why.empty()) c.why = w.str();
        return false;
    }
};

bool isRecordNamed(Type t, StringRef n) {
    auto r = dyn_cast<cir::RecordType>(t);
    return r && r.getName() && r.getName().getValue() == n;
}

void LiftCQueuesPass::runOnOperation() {
    module = getOperation();
    module.walk([&](cir::GetMemberOp gm) {
        auto pt = dyn_cast<cir::PointerType>(gm.getAddr().getType());
        auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
        if (rec) names.try_emplace({rec, static_cast<unsigned>(gm.getIndex())}, gm.getName().str());
    });
    if (!namedElsewhere.empty()) {
        if (auto buf = llvm::MemoryBuffer::getFile(namedElsewhere)) {
            llvm::SmallVector<StringRef, 64> lines;
            (*buf)->getBuffer().split(lines, '\n', -1, false);
            for (StringRef l : lines) elsewhere.insert(l.trim().str());
        }
    }

    // Structs with a pthread mutex and a condition variable, directly or in
    // a member struct, and the functions that take a pointer to them.
    std::vector<Candidate> cands;
    llvm::SetVector<Type> seen;
    module.walk([&](cir::FuncOp f) {
        if (f.isDeclaration()) return;
        for (Type t : f.getArgumentTypes()) {
            auto pt = dyn_cast<cir::PointerType>(t);
            auto rec = pt ? dyn_cast<cir::RecordType>(pt.getPointee()) : cir::RecordType();
            if (!rec || rec.isIncomplete() || !seen.insert(rec)) continue;
            bool m = false, cv = false;
            for (Type mt : rec.getMembers()) {
                m |= isRecordNamed(mt, "pthread_mutex_t");
                cv |= isRecordNamed(mt, "pthread_cond_t");
            }
            if (!m || !cv) continue;
            Candidate c;
            c.rec = rec;
            c.label = "struct " + (rec.getName() ? rec.getName().getValue().str() : std::string("<anonymous>"));
            cands.push_back(c);
        }
    });
    for (Candidate &c : cands) {
        module.walk([&](cir::FuncOp f) {
            if (f.isDeclaration()) return;
            for (auto [i, t] : llvm::enumerate(f.getArgumentTypes())) {
                auto pt = dyn_cast<cir::PointerType>(t);
                if (pt && pt.getPointee() == Type(c.rec)) {
                    Fn fn;
                    fn.f = f;
                    fn.q = static_cast<unsigned>(i);
                    fn.root = "P" + std::to_string(i);
                    c.fns.push_back(fn);
                    break;
                }
            }
        });
        analyse(c);
        if (!c.why.empty()) {
            emitRemark(c.fns.empty() ? module.getLoc() : c.fns.front().f.getLoc())
                << "wtc: C queue " << c.label << " not lifted: " << c.why;
            continue;
        }
        if (!rewrite) {
            InFlightDiagnostic d = emitRemark(c.fns.front().f.getLoc()) << "wtc: C queue " << c.label << " lifts (";
            bool first = true;
            for (Fn &fn : c.fns) {
                if (fn.role == Role::Helper) continue;
                d << (first ? "" : ", ") << fn.f.getSymName() << ": " << roleName(fn.role);
                first = false;
            }
            d << ")";
            continue;
        }
        lift(c);
    }
}

// Every function of the struct read and classified, the layout agreed, the
// proof's conditions checked.
void LiftCQueuesPass::analyse(Candidate &c) {
    if (c.fns.empty()) return (void)refuseWith(c, "(a) no function takes a pointer to it");
    if (!openWorld && !module.lookupSymbol<cir::FuncOp>("main"))
        return (void)refuseWith(c, "(a) the module has no main, so other units may touch the struct");
    if (openWorld) return (void)refuseWith(c, "(a) the module is compiled to be linked with units it does not see");
    // Members written by the operations (not by initialisation and
    // destruction): only those change while the queue is shared.
    for (Fn &fn : c.fns) {
        bool timed = false;
        fn.f.walk([&](cir::CallOp call) {
            cir::FuncOp callee = lookupCallee(call);
            timed |= callee && callee.getSymName() == "pthread_cond_timedwait";
        });
        if (timed) return (void)refuseWith(c, "(d) " + fn.f.getSymName().str() + " waits with a time limit");
    }
    std::set<std::string> mut;
    for (Fn &fn : c.fns) {
        Eval e(fn.f, fn.root, nullptr);
        std::vector<Path> ps;
        if (!e.run(ps)) return (void)refuseWith(c, "(a) " + fn.f.getSymName().str() + " " + e.why);
        bool lifecycle = false;
        for (const Path &p : ps)
            for (const Event &ev : p.events) lifecycle |= ev.kind == "minit" || ev.kind == "mdestroy";
        if (!lifecycle)
            for (const std::string &k : e.written)
                if (StringRef(k).starts_with(fn.root + ":")) mut.insert(k.substr(fn.root.size() + 1));
    }
    for (Fn &fn : c.fns) {
        std::set<std::string> keys;
        for (const std::string &k : mut) keys.insert(fn.root + ":" + k);
        // A caller's ring or an out parameter is the function's own: every
        // member of another root is mutable.
        for (BlockArgument a : fn.f.getBody().front().getArguments()) {
            const std::string r = "P" + std::to_string(a.getArgNumber());
            if (r == fn.root) continue;
            std::function<void(Type, std::string)> all = [&](Type t, std::string pre) {
                keys.insert(pre);
                if (auto rt = dyn_cast<cir::RecordType>(t))
                    for (auto [i, mt] : llvm::enumerate(rt.getMembers()))
                        all(mt, pre + (pre.back() == ':' ? "" : ".") + std::to_string(i));
            };
            if (auto pt = dyn_cast<cir::PointerType>(a.getType())) all(pt.getPointee(), r + ":");
        }
        Eval e(fn.f, fn.root, &keys);
        if (!e.run(fn.paths)) return (void)refuseWith(c, "(a) " + fn.f.getSymName().str() + " " + e.why);
        fn.written = e.written;
        fn.aborts = e.aborts;
        fn.lossy = e.lossyParams;
        mergePaths(fn.paths);
        if (fn.paths.empty()) return (void)refuseWith(c, "(a) " + fn.f.getSymName().str() + " always ends the program");
    }
    for (Fn &fn : c.fns)
        if (!classify(c, fn)) return;
    if (!confined(c)) return;
}

// The atoms of one root's member at a generation.
struct AtomName {
    int gen = -1;  // -1: invariant
    std::string root;
    std::vector<unsigned> path;
};
std::optional<AtomName> parseAtom(const Ex &e) {
    if (e->op != "atom" || e->name.size() < 3 || e->name[0] != 'f') return std::nullopt;
    StringRef s(e->name);
    AtomName a;
    StringRef g = s.drop_front().take_until([](char c) { return c == ':'; });
    if (!g.empty() && g.getAsInteger(10, a.gen)) return std::nullopt;
    s = s.drop_until([](char c) { return c == ':'; }).drop_front();
    auto [r, rest] = s.split(':');
    a.root = r.str();
    if (!rest.empty()) {
        llvm::SmallVector<StringRef, 4> parts;
        rest.split(parts, '.');
        for (StringRef x : parts) {
            unsigned v;
            if (x.getAsInteger(10, v)) return std::nullopt;
            a.path.push_back(v);
        }
    }
    return a;
}

// Truth tables over the facts a path may decide on.
struct Facts {
    const Candidate &c;
    const std::string &root;
    std::string unknown;  // the first expression not read
    Ex unknownEx;
    bool isMember(const Ex &e, const std::vector<unsigned> &path, int gen) const {
        auto a = parseAtom(e);
        return a && a->root == root && a->path == path && (gen < 0 || a->gen == gen || a->gen == -1);
    }
    int genOf(const Ex &e) const {
        auto a = parseAtom(e);
        return a ? a->gen : -2;
    }
    // A proposition's name, and whether the expression is it or its negation.
    std::optional<std::pair<std::string, bool>> prop(const Ex &e) {
        const Ring &r = c.ring;
        auto in = [&](const Ex &x) { return r.known && isMember(x, r.in, -1); };
        auto out = [&](const Ex &x) { return r.known && isMember(x, r.out, -1); };
        auto ended = [&](const Ex &x) { return c.counted && isMember(x, c.ended, -1); };
        auto prods = [&](const Ex &x) { return c.counted && isMember(x, c.producers, -1); };
        const std::string op = e->op;
        if (op == "eq" || op == "ne") {
            const bool pos = op == "eq";
            const Ex &x = e->kids[0], &y = e->kids[1];
            for (int s = 0; s < 2; ++s) {
                const Ex &a = s ? y : x, &b = s ? x : y;
                if (in(a) && out(b) && genOf(a) == genOf(b))
                    return std::pair{"empty" + std::to_string(genOf(a)), pos};
                if (a->op == "inc" && in(a->kids[0]) && out(b) && genOf(a->kids[0]) == genOf(b) &&
                    isMember(a->kids[1], r.s, -1))
                    return std::pair{"full" + std::to_string(genOf(b)), pos};
                if (ended(a) && prods(b)) {
                    c.endEq = true;
                    return std::pair{"ended" + std::to_string(genOf(a)), pos};
                }
                if (a->op == "add" && isK(a->kids[0], 1) && ended(a->kids[1]) && prods(b)) {
                    c.endEq = true;
                    return std::pair{"last" + std::to_string(genOf(a->kids[1])), pos};
                }
                if (a->key == "moved" && isK(b, 0)) return std::pair{std::string("moved"), !pos};
            }
        }
        if (op == "lt" || op == "le") {
            const Ex &x = e->kids[0], &y = e->kids[1];
            // e < n is "not ended", n <= e "ended"; e + 1 likewise "last".
            if (ended(x) && prods(y) && op == "lt") return std::pair{"ended" + std::to_string(genOf(x)), false};
            if (prods(x) && ended(y) && op == "le") return std::pair{"ended" + std::to_string(genOf(y)), true};
            if (x->op == "add" && isK(x->kids[0], 1) && ended(x->kids[1]) && prods(y) && op == "lt")
                return std::pair{"last" + std::to_string(genOf(x->kids[1])), false};
            if (prods(x) && y->op == "add" && isK(y->kids[0], 1) && ended(y->kids[1]) && op == "le")
                return std::pair{"last" + std::to_string(genOf(y->kids[1])), true};
            if (isK(x, 0) && y->key == "moved" && op == "lt") return std::pair{std::string("moved"), true};
        }
        return std::nullopt;
    }
    void collect(const Ex &e, std::set<std::string> &out) {
        if (isTrue(e) || isFalse(e)) return;
        if (e->op == "and" || e->op == "or" || e->op == "not" || e->op == "ite") {
            for (const Ex &k : e->kids) collect(k, out);
            return;
        }
        if (auto pr = prop(e)) out.insert(pr->first);
        else if (unknown.empty()) unknown = e->key, unknownEx = e;
    }
    std::optional<bool> truth(const Ex &e, const std::map<std::string, bool> &v) {
        if (isTrue(e)) return true;
        if (isFalse(e)) return false;
        if (e->op == "not") {
            auto a = truth(e->kids[0], v);
            return a ? std::optional<bool>(!*a) : std::nullopt;
        }
        if (e->op == "and" || e->op == "or") {
            auto a = truth(e->kids[0], v), b = truth(e->kids[1], v);
            if (!a || !b) return std::nullopt;
            return e->op == "and" ? *a && *b : *a || *b;
        }
        if (e->op == "ite") {
            auto a = truth(e->kids[0], v), x = truth(e->kids[1], v), y = truth(e->kids[2], v);
            if (!a || !x || !y) return std::nullopt;
            return *a ? *x : *y;
        }
        auto pr = prop(e);
        if (!pr) return std::nullopt;
        auto it = v.find(pr->first);
        if (it == v.end()) return std::nullopt;
        return it->second == pr->second;
    }
};

// Every assignment of the facts named.
std::vector<std::map<std::string, bool>> assignments(const std::set<std::string> &names) {
    std::vector<std::string> ns(names.begin(), names.end());
    std::vector<std::map<std::string, bool>> out;
    for (unsigned m = 0; m < (1u << ns.size()); ++m) {
        std::map<std::string, bool> a;
        for (size_t i = 0; i < ns.size(); ++i) a[ns[i]] = (m >> i) & 1;
        out.push_back(a);
    }
    return out;
}

std::vector<unsigned> keyPath(const std::string &key) {
    std::vector<unsigned> p;
    StringRef rest = StringRef(key).split(':').second;
    if (rest.empty()) return p;
    llvm::SmallVector<StringRef, 4> parts;
    rest.split(parts, '.');
    for (StringRef x : parts) {
        unsigned v = 0;
        x.getAsInteger(10, v);
        p.push_back(v);
    }
    return p;
}

// Memory outside the queue a function writes, for a remark: "*parameter
// 2", or "member 1 of *parameter 2".
std::string placeOf(const std::string &key) {
    auto [root, rest] = StringRef(key).split(':');
    std::string where = root.starts_with("P") ? "*parameter " + root.drop_front().str() : root.str();
    return rest.empty() ? where : "member " + rest.str() + " of " + where;
}

std::optional<unsigned> paramOf(const Ex &e) {
    unsigned i;
    if (e->op != "atom" || e->name.size() < 2 || e->name[0] != 'P' || StringRef(e->name).drop_front().getAsInteger(10, i))
        return std::nullopt;
    return i;
}

// An expression as a remark shows it: members by name, as of the lock or
// after a wait, parameters by number.
std::string readable(const Candidate &c, const Ex &e, const std::string &root,
                     const llvm::DenseMap<std::pair<Type, unsigned>, std::string> &names) {
    static const std::map<std::string, std::string> infix{{"eq", " == "}, {"ne", " != "}, {"lt", " < "}, {"le", " <= "},
                                                          {"add", " + "}, {"sub", " - "}, {"mul", " * "}, {"rem", " % "},
                                                          {"and", " && "}, {"or", " || "}};
    if (e->op == "k") return std::to_string(e->k);
    if (e->op == "atom") {
        if (auto a = parseAtom(e)) {
            std::string n = a->root == root ? "q->" + fieldName(c.rec, a->path, names) : a->root + ":" + pathStr(a->path);
            return a->gen > 0 ? n + " (after the wait)" : n;
        }
        if (auto p = paramOf(e)) return "parameter " + std::to_string(*p);
        return e->name;
    }
    if (auto it = infix.find(e->op); it != infix.end() && e->kids.size() == 2)
        return "(" + readable(c, e->kids[0], root, names) + it->second + readable(c, e->kids[1], root, names) + ")";
    if (e->op == "not") return "!" + readable(c, e->kids[0], root, names);
    std::string s = e->op + "(";
    for (size_t i = 0; i < e->kids.size(); ++i) s += (i ? ", " : "") + readable(c, e->kids[i], root, names);
    return s + ")";
}

std::string LiftCQueuesPass::readable(Candidate &c, const Ex &e, const std::string &root) {
    return ::readable(c, e, root, names);
}

bool LiftCQueuesPass::classify(Candidate &c, Fn &fn) {
    const std::string fname = fn.f.getSymName().str();
    auto no = [&](const Twine &w) { return refuseWith(c, w); };
    auto has = [](const Path &p, StringRef kind) {
        return llvm::any_of(p.events, [&](const Event &e) { return e.kind == kind; });
    };
    const Path &p0 = fn.paths.front();
    auto anyEv = [&](StringRef kind) { return llvm::any_of(fn.paths, [&](const Path &p) { return has(p, kind); }); };
    auto rootOf = [](const Ex &e) { auto a = parseAtom(e); return a ? a->root : std::string(); };

    // Initialisation and destruction.
    if (anyEv("minit")) {
        fn.role = Role::Init;
        if (fn.paths.size() != 1) return no("(a) " + fname + " initialises the queue on several paths");
        const Path &p = p0;
        for (auto &[key, v] : p.fields)
            if (!StringRef(key).starts_with(fn.root + ":"))
                return no("(c) " + fname + " writes " + placeOf(key) +
                          ", outside the queue");
        // What it returns must be what the substitute returns: a constant.
        if (p.ret && p.ret->op != "k")
            return no("(a) " + fname + " returns " + readable(c, p.ret, fn.root) + ", not a constant");
        if (p.ret) fn.constRet = p.ret->k;
        // The allocation, the slot count and the indices.
        Ex alloc;
        for (const Event &e : p.events) {
            if (e.kind == "malloc") alloc = e.x;
            else if (e.kind != "minit" && e.kind != "cinit")
                return no("(a) " + fname + " does more than initialise the mutex, the variables and the ring");
        }
        if (!alloc) return no("(e) " + fname + " allocates no ring");
        std::string dKey, sKey;
        for (auto &[key, v] : p.fields)
            if (v->op == "malloc") dKey = key;
        if (dKey.empty()) return no("(e) " + fname + " keeps the ring's allocation nowhere");
        // malloc(sizeof(void *) * slots)
        Ex slots;
        if (alloc->op == "mul" && isK(alloc->kids[0], 8)) slots = alloc->kids[1];
        else if (alloc->op == "mul" && isK(alloc->kids[1], 8)) slots = alloc->kids[0];
        if (!slots) return no("(e) " + fname + " allocates the ring as " + alloc->key + ", not as slots of pointers");
        for (auto &[key, v] : p.fields)
            if (same(v, slots)) sKey = key;
        if (sKey.empty()) return no("(e) " + fname + " keeps the ring's slot count nowhere");
        // slots = P + k
        std::optional<unsigned> sp = paramOf(slots);
        int64_t off = 0;
        if (!sp && slots->op == "add") {
            for (int s = 0; s < 2; ++s)
                if (slots->kids[s]->op == "k" && paramOf(slots->kids[1 - s])) {
                    sp = paramOf(slots->kids[1 - s]);
                    off = slots->kids[s]->k;
                }
        }
        if (!sp) return no("(e) " + fname + " sets the slot count to " + slots->key + ", not a parameter plus a constant");
        c.capParam = *sp;
        c.capOffset = off - 1;
        for (auto &[key, v] : p.fields) {
            if (key == dKey || key == sKey) continue;
            if (isK(v, 0)) continue;
            auto pp = paramOf(v);
            if (!pp || c.hasProducers)
                return no("(a) " + fname + " sets " + field(c, keyPath(key)) + " to " + readable(c, v, fn.root));
            c.hasProducers = true;
            c.prodParam = *pp;
            c.producers = keyPath(key);
        }
        // Kept for the layout check.
        c.ring.d = keyPath(dKey);
        c.ring.s = keyPath(sKey);
        fn.written.clear();
        for (auto &[key, v] : p.fields) fn.written.insert(key);
        return true;
    }
    if (anyEv("mdestroy")) {
        fn.role = Role::Destroy;
        for (const Path &p : fn.paths) {
            for (const Event &e : p.events) {
                if (e.kind != "mdestroy" && e.kind != "cdestroy" && e.kind != "free")
                    return no("(a) " + fname + " does more than destroy the mutex, the variables and the ring");
                if (e.kind == "free" && !parseAtom(e.x)) return no("(a) " + fname + " frees " + e.x->key);
                if (e.kind == "free") fn.freed.push_back(parseAtom(e.x)->path);
            }
            for (auto &[key, v] : p.fields) {
                if (!StringRef(key).starts_with(fn.root + ":"))
                    return no("(c) " + fname + " writes " + placeOf(key) +
                              ", outside the queue");
                if (v->key != "null" && !isK(v, 0))
                    return no("(a) " + fname + " sets " + field(c, keyPath(key)) + " to " + readable(c, v, fn.root));
            }
            if (p.ret && p.ret->op != "k")
                return no("(a) " + fname + " returns " + readable(c, p.ret, fn.root) + ", not a constant");
            if (p.ret && fn.constRet && *fn.constRet != p.ret->k)
                return no("(a) " + fname + " returns different constants");
            if (p.ret) fn.constRet = p.ret->k;
        }
        return true;
    }
    // A helper: no events, nothing written, a value returned.
    if (llvm::all_of(fn.paths, [&](const Path &p) { return p.events.empty() && p.fields.empty(); })) {
        fn.role = Role::Helper;
        return true;
    }

    // The operations: one lock, held throughout.
    std::string m;
    for (const Path &p : fn.paths) {
        if (p.unlockedAccess) return no("(b) " + fname + " touches the queue without its mutex");
        if (p.events.size() < 2 || p.events.front().kind != "lock" || p.events.back().kind != "unlock" ||
            p.events.front().a != p.events.back().a)
            return no("(b) " + fname + " does not hold the mutex from its first touch of the queue to its last");
        for (size_t i = 1; i + 1 < p.events.size(); ++i)
            if (p.events[i].kind == "lock" || p.events[i].kind == "unlock")
                return no("(b) " + fname + " lets the mutex go in the middle");
        if (!m.empty() && m != p.events.front().a) return no("(b) " + fname + " locks two mutexes");
        m = p.events.front().a;
    }
    if (!StringRef(m).starts_with(fn.root + ":")) return no("(c) " + fname + " locks a mutex not the struct's");
    // A wait releases the mutex it is given: the one held, the struct's;
    // and it waits on a variable of the struct's. A signal of anything
    // else would be dropped with the body.
    for (const Path &p : fn.paths)
        for (const Event &e : p.events) {
            if (e.kind == "wait" && e.b != m)
                return no("(b) " + fname + " waits with a mutex other than the one it holds");
            if ((e.kind == "wait" || e.kind == "signal" || e.kind == "broadcast") &&
                !StringRef(e.a).starts_with(fn.root + ":"))
                return no("(c) " + fname + " " + (e.kind == "wait" ? "waits on" : "wakes") +
                          " a condition variable outside the queue");
        }
    const std::vector<unsigned> mp = keyPath(m);
    if (!c.mutex.empty() && c.mutex != mp) return no("(b) the struct's functions lock different mutexes");
    c.mutex = mp;

    // What it is, by what it does to elements.
    const bool transfer = anyEv("transfer"), estore = anyEv("estore"), eload = anyEv("eload");
    if (transfer) {
        const Event *t = nullptr;
        for (const Path &p : fn.paths)
            for (const Event &e : p.events)
                if (e.kind == "transfer") t = &e;
        if (t->dst.root == fn.root && t->src.root != fn.root) {
            fn.role = Role::PushBatch;
            fn.ring = t->src;
        } else if (t->src.root == fn.root && t->dst.root != fn.root) {
            fn.role = Role::PopBatch;
            fn.ring = t->dst;
        } else {
            return no("(e) " + fname + " moves elements within the queue");
        }
        Ring r;
        const Event::Ring &q = fn.role == Role::PushBatch ? t->dst : t->src;
        r.d = q.d, r.in = q.in, r.out = q.out, r.s = q.s, r.known = true;
        if (c.ring.known && !(c.ring == r)) return no("(e) " + fname + " reads the ring differently");
        c.ring = r;
        auto rp = paramOf(atom(fn.ring.root));
        auto lp = paramOf(t->x);
        if (!rp || !lp) return no("(c) " + fname + " moves elements to or from a ring that is not a parameter");
        fn.ringParam = *rp;
        fn.limitParam = *lp;
    } else if (estore && !eload) {
        fn.role = Role::Push;
    } else if (eload && !estore) {
        fn.role = Role::Pop;
    } else if (!estore && !eload) {
        fn.role = Role::Terminate;
    } else {
        return no("(e) " + fname + " both reads and writes elements");
    }
    return true;
}

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

// The roles agreed, then each function checked against its operation.
bool LiftCQueuesPass::confined(Candidate &c) {
    auto no = [&](const Twine &w) { return refuseWith(c, w); };
    unsigned inits = 0;
    Fn *init = nullptr;
    for (Fn &fn : c.fns) {
        if (fn.role == Role::Init) {
            ++inits;
            init = &fn;
        }
    }
    if (inits != 1) return no("(a) " + std::to_string(inits) + " functions initialise the struct");
    if (auto uses = SymbolTable::getSymbolUses(init->f, module)) {
        for (const SymbolTable::SymbolUse &u : *uses) {
            auto call = dyn_cast<cir::CallOp>(u.getUser());
            if (!call || lookupCallee(call) != init->f)
                return no("(a) " + init->f.getSymName() + "'s address is taken");
            init->calls.push_back(call);
        }
    } else {
        return no("(a) " + init->f.getSymName() + " is used in a way the pass cannot follow");
    }
    for (Fn &fn : c.fns)
        for (const auto &fp : fn.freed)
            if (fp != c.ring.d) return no("(a) " + fn.f.getSymName() + " frees " + field(c, fp) + ", not the ring");
    // The layout, from the element operations.
    for (Fn &fn : c.fns) {
        if (fn.role != Role::Push && fn.role != Role::Pop) continue;
        for (const Path &p : fn.paths)
            for (const Event &e : p.events) {
                if (e.kind != "estore" && e.kind != "eload") continue;
                auto d = parseAtom(e.x), i = parseAtom(e.y);
                if (!d || !i || d->root != fn.root || i->root != fn.root)
                    return no("(e) " + fn.f.getSymName() + " reaches elements other than the ring's");
                std::vector<unsigned> &idx = fn.role == Role::Push ? c.ring.in : c.ring.out;
                if ((!c.ring.d.empty() && c.ring.d != d->path) || (!idx.empty() && idx != i->path))
                    return no("(e) " + fn.f.getSymName() + " reads the ring differently");
                c.ring.d = d->path;
                idx = i->path;
            }
    }
    if (c.ring.in.empty() || c.ring.out.empty() || c.ring.d.empty() || c.ring.s.empty())
        return no("(e) no function both puts elements in and takes them out");
    c.ring.known = true;
    // The substitute keeps pointers: a ring of anything else (a long has
    // the size of one) is refused.
    {
        Type t = c.rec;
        for (unsigned i : c.ring.d) {
            auto r = dyn_cast<cir::RecordType>(t);
            t = r && i < r.getMembers().size() ? r.getMembers()[i] : Type();
        }
        auto dt = dyn_cast_or_null<cir::PointerType>(t);
        if (!dt || !isa<cir::PointerType>(dt.getPointee()))
            return no("(e) the ring " + field(c, c.ring.d) + " holds something other than pointers");
    }
    // Where the program ends itself (an assertion, abort, exit): the
    // substitute makes none of those checks, so only one is allowed, an
    // initialisation's on its allocation, which the substitute's own
    // allocation makes (it ends the program when it fails).
    for (Fn &fn : c.fns)
        for (const std::vector<Lit> &a : fn.aborts) {
            std::function<bool(const Ex &)> allocOnly = [&](const Ex &e) {
                if (e->op == "malloc" || e->op == "k" || e->key == "null" || isTrue(e) || isFalse(e)) return true;
                if (e->op == "atom") return false;
                return llvm::all_of(e->kids, allocOnly);
            };
            bool ok = fn.role == Role::Init;
            std::string when;
            for (const Lit &l : a) {
                if (!l.fork) continue;
                ok = ok && allocOnly(l.e);
                when += (when.empty() ? "" : " && ") + readable(c, l.pol ? l.e : notE(l.e), fn.root);
            }
            if (!ok)
                return no("(d) " + fn.f.getSymName() + " ends the program " +
                          (when.empty() ? std::string("unconditionally") : "when " + when) +
                          ", a check the substitute does not make");
        }
    // The end count: what terminate increments; the producers: what init
    // sets from a parameter.
    for (Fn &fn : c.fns) {
        if (fn.role != Role::Terminate) continue;
        for (const Path &p : fn.paths) {
            if (p.fields.empty())
                return no("(a) " + fn.f.getSymName() + " takes the mutex but is none of the queue's operations");
            if (p.fields.size() != 1) return no("(c) " + fn.f.getSymName() + " changes more than one count");
            auto &[key, v] = *p.fields.begin();
            if (v->op != "add" || !isK(v->kids[0], 1) || !parseAtom(v->kids[1]) ||
                parseAtom(v->kids[1])->path != keyPath(key))
                return no("(c) " + fn.f.getSymName() + " sets " + field(c, keyPath(key)) + " to " +
                          readable(c, v, fn.root));
            if (!c.ended.empty() && c.ended != keyPath(key)) return no("(c) two functions count different things");
            c.ended = keyPath(key);
        }
    }
    c.counted = !c.ended.empty();
    if (c.counted && !c.hasProducers) return no("(d) the end count is compared with nothing initialisation sets");
    if (!c.counted && c.hasProducers) return no("(c) initialisation sets " + field(c, c.producers) + ", which nothing counts against");
    // Initialisation: the indices and the count zero.
    {
        std::set<std::vector<unsigned>> want{c.ring.d, c.ring.s, c.ring.in, c.ring.out};
        if (c.counted) want.insert(c.ended), want.insert(c.producers);
        std::set<std::vector<unsigned>> got;
        for (const std::string &k : init->written) got.insert(keyPath(k));
        if (got != want) return no("(e) " + init->f.getSymName() + " sets other members than the ring's, or not all of them");
        for (auto &[key, v] : init->paths.front().fields) {
            const auto kp = keyPath(key);
            if ((kp == c.ring.in || kp == c.ring.out || (c.counted && kp == c.ended)) && !isK(v, 0))
                return no("(e) " + init->f.getSymName() + " starts " + field(c, kp) + " at " + v->key);
        }
    }
    // Each operation against its specification.
    for (Fn &fn : c.fns) {
        if (fn.role == Role::Init || fn.role == Role::Destroy || fn.role == Role::Helper) continue;
        const std::string fname = fn.f.getSymName().str();
        Facts facts{c, fn.root, ""};
        // Members written: only what the operation moves.
        std::set<std::vector<unsigned>> allowed;
        if (fn.role == Role::Push) allowed = {c.ring.in};
        if (fn.role == Role::Pop) allowed = {c.ring.out};
        if (fn.role == Role::Terminate) allowed = {c.ended};
        for (const Path &p : fn.paths)
            for (auto &[key, v] : p.fields) {
                if (!StringRef(key).starts_with(fn.root + ":")) {
                    // Memory outside the queue: only the element handed
                    // to the caller, `*out = q->data[q->tail]`, on the
                    // path that takes it; checked with the pop below.
                    const bool takes = llvm::any_of(p.events, [](const Event &e) { return e.kind == "eload"; });
                    if (fn.role == Role::Pop && takes && keyPath(key).empty() && v->op == "eload") continue;
                    return no("(c) " + fname + " writes " + placeOf(key) + ", outside the queue" +
                              (fn.role == Role::Pop && !takes ? ", when the queue is finished" : ""));
                }
                if (!allowed.count(keyPath(key)))
                    return no("(c) " + fname + " writes " + field(c, keyPath(key)) + ", which the mutex guards too");
            }
        // The waits, the notifications and what each path decides on.
        std::set<std::string> props;
        for (const Path &p : fn.paths) {
            for (const Lit &l : p.conds) facts.collect(l.e, props);
            for (const Event &e : p.events)
                if (e.kind == "wait") facts.collect(e.x, props);
        }
        if (facts.unknownEx) return no("(d) " + fname + " decides on " + readable(c, facts.unknownEx, fn.root));
        auto waitOf = [&](const Path &p) -> const Event * {
            const Event *w = nullptr;
            for (const Event &e : p.events)
                if (e.kind == "wait") {
                    if (w) return nullptr;
                    w = &e;
                }
            return w;
        };
        auto notifies = [&](const Path &p, const std::vector<unsigned> &cv) {
            return llvm::any_of(p.events, [&](const Event &e) {
                return (e.kind == "signal" || e.kind == "broadcast") && keyPath(e.a) == cv &&
                       StringRef(e.a).starts_with(fn.root + ":");
            });
        };
        // The wait's predicate as a table over the facts at generation 0.
        auto waitIs = [&](const Event &w, std::function<bool(const std::map<std::string, bool> &)> want) {
            std::set<std::string> ns;
            Facts f2{c, fn.root, ""};
            f2.collect(w.x, ns);
            ns.insert("empty0"), ns.insert("full0");
            if (c.counted) ns.insert("ended0");
            for (auto &a : assignments(ns)) {
                auto t = f2.truth(w.x, a);
                if (!t || *t != want(a)) return false;
            }
            return true;
        };
        // The facts after the wait, generation 1.
        auto table = [&](const Path &p, const std::map<std::string, bool> &a) -> std::optional<bool> {
            for (const Lit &l : p.conds) {
                auto t = facts.truth(l.e, a);
                if (!t) return std::nullopt;
                if (*t != l.pol) return false;
            }
            return true;
        };
        std::set<std::string> ns = props;
        if (fn.role == Role::Pop || fn.role == Role::PopBatch) {
            ns.insert("empty1");
            if (c.counted) ns.insert("ended1");
        }
        const auto all = assignments(ns);
        switch (fn.role) {
        case Role::Push:
        case Role::PushBatch: {
            for (const Path &p : fn.paths) {
                const Event *w = waitOf(p);
                if (!w) return no("(d) " + fname + " does not wait while the ring is full, or waits twice");
                if (!waitIs(*w, [](auto &a) { return a.at("full0"); }))
                    return no("(d) " + fname + " waits while " + readable(c, w->x, fn.root) + ", not while the ring is full");
                if (!c.notFull.empty() && c.notFull != keyPath(w->a)) return no("(d) pushes wait on different variables");
                c.notFull = keyPath(w->a);
            }
            if (fn.role == Role::Push) {
                if (fn.paths.size() != 1) return no("(d) " + fname + " has several outcomes");
                const Path &p = fn.paths.front();
                const Event *st = nullptr;
                for (const Event &e : p.events)
                    if (e.kind == "estore") st = &e;
                auto idx = parseAtom(st->y);
                if (!idx || idx->gen != 1) return no("(d) " + fname + " stores before its wait ends");
                auto v = paramOf(st->z);
                if (!v) return no("(e) " + fname + " stores " + st->z->key + ", not its parameter");
                fn.valueParam = *v;
                auto adv = p.fields.find(fn.root + ":" + pathStr(c.ring.in));
                if (adv == p.fields.end() || adv->second->op != "inc" || !same(adv->second->kids[0], st->y))
                    return no("(e) " + fname + " does not advance the index it stores at by one");
                if (!facts.isMember(adv->second->kids[1], c.ring.s, -1))
                    return no("(e) " + fname + " wraps the index at " + readable(c, adv->second->kids[1], fn.root) +
                              ", not at the slot count " + field(c, c.ring.s));
                if (p.ret) {
                    if (p.ret->op != "k") return no("(a) " + fname + " returns " + p.ret->key);
                    fn.pushRet = p.ret->k;
                }
            } else {
                for (const Path &p : fn.paths) {
                    if (!p.ret || p.ret->key != "moved") return no("(a) " + fname + " does not return how many it moved");
                    for (const Lit &l : p.conds) facts.collect(l.e, ns);
                }
            }
            break;
        }
        case Role::Pop:
        case Role::PopBatch: {
            Ex take, closed;
            for (const Path &p : fn.paths) {
                const Event *w = waitOf(p);
                if (!w) return no("(d) " + fname + " does not wait while the ring is empty, or waits twice");
                const bool counted = c.counted;
                if (!waitIs(*w, [&](auto &a) { return a.at("empty0") && !(counted && a.at("ended0")); }))
                    return no("(d) " + fname + " waits while " + readable(c, w->x, fn.root) +
                              (counted ? ", not while the ring is empty and producers remain" : ", not while the ring is empty"));
                if (!c.notEmpty.empty() && c.notEmpty != keyPath(w->a)) return no("(d) pops wait on different variables");
                c.notEmpty = keyPath(w->a);
            }
            // Each path takes or answers "finished"; which is which, by the
            // facts after the wait.
            for (auto &a : all) {
                // After the wait: not (empty and not ended).
                if (a.at("empty1") && !(c.counted && a.at("ended1"))) continue;
                int takers = 0, closers = 0;
                for (const Path &p : fn.paths) {
                    auto t = table(p, a);
                    if (!t) return no("(d) " + fname + " decides on something the pass cannot read");
                    if (!*t) continue;
                    const bool takes = llvm::any_of(p.events, [](const Event &e) {
                        return e.kind == "eload" || e.kind == "transfer";
                    });
                    (takes ? takers : closers)++;
                }
                if (takers + closers != 1) return no("(d) " + fname + " has overlapping or missing outcomes");
                if (a.at("empty1") ? closers != 1 : takers != 1)
                    return no(a.at("empty1") ? "(d) " + fname + " takes from an empty ring"
                                             : "(d) " + fname + " gives up while elements are queued");
            }
            for (const Path &p : fn.paths) {
                const bool takes = llvm::any_of(p.events, [](const Event &e) { return e.kind == "eload" || e.kind == "transfer"; });
                if (!p.ret) return no("(a) " + fname + " returns nothing");
                if (!takes) {
                    if (p.ret->op != "k") return no("(a) " + fname + " answers \"finished\" with " + p.ret->key);
                    if (closed && !same(closed, p.ret)) return no("(a) " + fname + " answers \"finished\" in two ways");
                    closed = p.ret;
                    for (auto &[key, v] : p.fields)
                        if (StringRef(key).starts_with(fn.root + ":")) return no("(c) " + fname + " changes the queue when it is finished");
                    continue;
                }
                if (fn.role == Role::Pop) {
                    if (p.ret->op != "k") return no("(a) " + fname + " returns " + p.ret->key);
                    if (take && !same(take, p.ret)) return no("(a) " + fname + " answers in two ways");
                    take = p.ret;
                    const Event *ld = nullptr;
                    for (const Event &e : p.events)
                        if (e.kind == "eload") ld = &e;
                    auto idx = parseAtom(ld->y);
                    if (!idx || idx->gen != 1) return no("(d) " + fname + " reads before its wait ends");
                    // *out = the element; the index advanced.
                    std::optional<unsigned> outP;
                    for (auto &[key, v] : p.fields)
                        if (!StringRef(key).starts_with(fn.root + ":") && same(v, mk("eload", {ld->x, ld->y})) &&
                            keyPath(key).empty())
                            outP = paramOf(atom(StringRef(key).split(':').first.str()));
                    if (!outP) return no("(e) " + fname + " does not hand the element to its caller");
                    fn.outParam = *outP;
                    auto adv = p.fields.find(fn.root + ":" + pathStr(c.ring.out));
                    if (adv == p.fields.end() || adv->second->op != "inc" || !same(adv->second->kids[0], ld->y))
                        return no("(e) " + fname + " does not advance the index it takes from by one");
                    if (!facts.isMember(adv->second->kids[1], c.ring.s, -1))
                        return no("(e) " + fname + " wraps the index at " +
                                  readable(c, adv->second->kids[1], fn.root) + ", not at the slot count " +
                                  field(c, c.ring.s));
                } else if (!p.ret || p.ret->key != "moved") {
                    return no("(a) " + fname + " does not return how many it moved");
                }
            }
            if (c.counted && !closed) return no("(d) " + fname + " never answers \"finished\"");
            if (fn.role == Role::Pop && closed && take && same(take, closed))
                return no("(a) " + fname + " answers the same whether it took an element or not");
            fn.takeRet = take;
            fn.closedRet = closed;
            break;
        }
        case Role::Terminate: {
            for (const Path &p : fn.paths) {
                if (llvm::any_of(p.events, [](const Event &e) { return e.kind == "wait"; }))
                    return no("(d) " + fname + " waits");
                if (p.ret && p.ret->op != "k")
                    return no("(a) " + fname + " returns " + readable(c, p.ret, fn.root) + ", not a constant");
                if (p.ret && fn.constRet && *fn.constRet != p.ret->k)
                    return no("(a) " + fname + " returns different constants");
                if (p.ret) fn.constRet = p.ret->k;
            }
            break;
        }
        default:
            break;
        }
    }
    // Notifications: every push wakes the pops' variable, every pop the
    // pushes', the last termination the pops'; a batch only when it moved
    // something.
    for (Fn &fn : c.fns) {
        const std::string fname = fn.f.getSymName().str();
        auto notifies = [&](const Path &p, const std::vector<unsigned> &cv) {
            return llvm::any_of(p.events, [&](const Event &e) {
                return (e.kind == "signal" || e.kind == "broadcast") && keyPath(e.a) == cv;
            });
        };
        Facts facts{c, fn.root, ""};
        for (const Path &p : fn.paths) {
            std::map<std::string, bool> dummy;
            const bool takes = llvm::any_of(p.events, [](const Event &e) { return e.kind == "eload" || e.kind == "transfer"; });
            auto moved = [&]() {
                // Whether the path's conditions allow nothing moved.
                for (const Lit &l : p.conds) {
                    auto pr = facts.prop(l.e);
                    if (pr && pr->first == "moved") return pr->second == l.pol;
                }
                return true;
            };
            switch (fn.role) {
            case Role::Push:
                if (!notifies(p, c.notEmpty)) return no("(d) " + fname + " does not wake the pops");
                break;
            case Role::PushBatch:
                if (moved() && !notifies(p, c.notEmpty)) return no("(d) " + fname + " does not wake the pops when it moved something");
                break;
            case Role::Pop:
                if (takes && !c.notFull.empty() && !notifies(p, c.notFull)) return no("(d) " + fname + " does not wake the pushes");
                break;
            case Role::PopBatch:
                if (takes && moved() && !c.notFull.empty() && !notifies(p, c.notFull))
                    return no("(d) " + fname + " does not wake the pushes when it moved something");
                break;
            case Role::Terminate: {
                // On the path where the count reaches the producers.
                bool last = true;
                for (const Lit &l : p.conds) {
                    auto pr = facts.prop(l.e);
                    if (pr && StringRef(pr->first).starts_with("last")) last = pr->second == l.pol;
                    else return no("(d) " + fname + " decides on " + l.e->key);
                }
                if (last && !notifies(p, c.notEmpty)) return no("(d) " + fname + " does not wake the pops at the last termination");
                break;
            }
            default:
                break;
            }
        }
    }
    if (c.notEmpty.empty()) return no("(d) nothing pops");
    // Helpers: only the operations call them.
    std::set<Operation *> ops;
    for (Fn &fn : c.fns) ops.insert(fn.f);
    for (Fn &fn : c.fns) {
        if (fn.role != Role::Helper) continue;
        auto uses = SymbolTable::getSymbolUses(fn.f, module);
        if (!uses) return no("(a) " + fn.f.getSymName() + " is used in a way the pass cannot follow");
        for (const SymbolTable::SymbolUse &u : *uses) {
            auto call = dyn_cast<cir::CallOp>(u.getUser());
            if (!call || !ops.count(call->getParentOfType<cir::FuncOp>()))
                return no("(a) " + fn.f.getSymName() + " is called from outside the queue's functions");
        }
    }
    // The struct's members and pointers to it, outside its functions.
    Type ptrTy = cir::PointerType::get(c.rec);
    std::string bad;
    module.walk([&](Operation *op) {
        if (!bad.empty()) return;
        auto fn = op->getParentOfType<cir::FuncOp>();
        if (fn && ops.count(fn)) return;
        if (auto gm = dyn_cast<cir::GetMemberOp>(op))
            if (gm.getAddr().getType() == ptrTy) {
                bad = "(a) " + (fn ? fn.getSymName().str() : std::string("a global")) + " touches " +
                      field(c, {static_cast<unsigned>(gm.getIndex())});
                return;
            }
        for (Value v : op->getResults()) {
            if (v.getType() == Type(c.rec)) {
                bad = "(a) " + (fn ? fn.getSymName().str() : std::string("a global")) + " copies the struct";
                return;
            }
            if (v.getType() != ptrTy) continue;
            for (OpOperand &u : v.getUses()) {
                Operation *user = u.getOwner();
                if (auto gm = dyn_cast<cir::GetMemberOp>(user)) {
                    bad = "(a) " + (fn ? fn.getSymName().str() : std::string("a global")) + " touches " +
                          field(c, {static_cast<unsigned>(gm.getIndex())});
                    return;
                }
                // The pointer stored: only into a local slot or a global
                // that holds pointers to the struct and nothing else, whose
                // loads are pointers to the struct and are checked here as
                // every other one is. Stored anywhere else (a member of
                // another struct, through a pointer), it reaches code that
                // the walk does not follow.
                if (auto st = dyn_cast<cir::StoreOp>(user); st && st.getValue() == v) {
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
    // Nothing another unit sees.
    for (Fn &fn : c.fns)
        if (elsewhere.count(fn.f.getSymName().str()))
            return no("(a) " + fn.f.getSymName() + " is named by another file linked with this one");
    for (auto g : module.getOps<cir::GlobalOp>()) {
        Type t = g.getSymType();
        while (auto a = dyn_cast<cir::ArrayType>(t)) t = a.getElementType();
        if ((t == Type(c.rec) || t == ptrTy) && elsewhere.count(g.getSymName().str()))
            return no("(a) " + g.getSymName() + " is named by another file linked with this one");
        // A queue the program initialised statically, `= { ... }`, is
        // never built by the substitute's init: refused. Zero, or no
        // initialiser, is the object init then builds.
        if (t == Type(c.rec)) {
            auto iv = g.getInitialValue();
            if (iv && !isa<cir::ZeroAttr>(*iv))
                return no("(a) " + g.getSymName() + " is initialised statically, not by " + init->f.getSymName().str());
        }
    }
    // A batch's ring: the caller's own.
    for (Fn &fn : c.fns)
        if ((fn.role == Role::PushBatch || fn.role == Role::PopBatch) && !localRing(c, fn)) return false;
    // The indices are ints; the slot count any integer.
    auto typeAt = [&](Type t, const std::vector<unsigned> &path) -> Type {
        for (unsigned i : path) {
            auto r = dyn_cast<cir::RecordType>(t);
            if (!r || i >= r.getMembers().size()) return Type();
            t = r.getMembers()[i];
        }
        return t;
    };
    for (Fn &fn : c.fns) {
        if (fn.role != Role::PushBatch && fn.role != Role::PopBatch) continue;
        auto pt = dyn_cast<cir::PointerType>(fn.f.getArgumentTypes()[fn.ringParam]);
        if (!pt) return no("(c) " + fn.f.getSymName() + "'s ring is not a pointer");
        auto i32 = [](Type t) { auto it = dyn_cast_or_null<cir::IntType>(t); return it && it.getWidth() == 32; };
        if (!i32(typeAt(pt.getPointee(), fn.ring.in)) || !i32(typeAt(pt.getPointee(), fn.ring.out)) ||
            !isa_and_nonnull<cir::IntType>(typeAt(pt.getPointee(), fn.ring.s)))
            return no("(c) " + fn.f.getSymName() + "'s ring has indices that are not ints");
        auto dt = dyn_cast_or_null<cir::PointerType>(typeAt(pt.getPointee(), fn.ring.d));
        if (!dt || !isa<cir::PointerType>(dt.getPointee()))
            return no("(c) " + fn.f.getSymName() + "'s ring does not hold pointers");
        // The limit, compared with the count as the program wrote it: the
        // runtime takes it as a count of 64 bits, every value of the
        // parameter's type kept (a size_t of -1 is no limit, an int below
        // zero none). A conversion on the way that changes values (an int
        // made unsigned, a size_t made int) would compare something else.
        if (!isa<cir::IntType>(fn.f.getArgumentTypes()[fn.limitParam]))
            return no("(c) " + fn.f.getSymName() + "'s limit is not an integer");
        if (fn.lossy.count(fn.limitParam))
            return no("(c) " + fn.f.getSymName() + " converts its limit, parameter " + std::to_string(fn.limitParam) +
                      ", to a type that does not hold all of its values");
    }
    return true;
}

// Every call of a batch passes the address of a local of the caller, which
// goes to nothing but direct calls: the calling thread's own ring.
bool LiftCQueuesPass::localRing(Candidate &c, Fn &fn) {
    auto uses = SymbolTable::getSymbolUses(fn.f, module);
    if (!uses) return refuseWith(c, "(c) " + fn.f.getSymName() + " is used in a way the pass cannot follow");
    for (const SymbolTable::SymbolUse &u : *uses) {
        auto call = dyn_cast<cir::CallOp>(u.getUser());
        if (!call) return refuseWith(c, "(c) " + fn.f.getSymName() + "'s address is taken");
        Value r = call.getArgOperands()[fn.ringParam];
        auto a = r.getDefiningOp<cir::AllocaOp>();
        if (!a) return refuseWith(c, "(c) a call of " + fn.f.getSymName() + " passes a ring that is not the caller's local");
        for (OpOperand &use : a->getUses()) {
            auto other = dyn_cast<cir::CallOp>(use.getOwner());
            if (other && lookupCallee(other) && getSemanticKind(lookupCallee(other)) != SemanticOpKind::ThreadSpawn)
                continue;
            if (isa<cir::GetMemberOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(use.getOwner())) continue;
            return refuseWith(c, "(c) the ring a call of " + fn.f.getSymName() + " passes may be reached by another thread (" +
                                     use.getOwner()->getName().getStringRef() + ")");
        }
    }
    return true;
}

cir::FuncOp LiftCQueuesPass::runtime(StringRef name, llvm::ArrayRef<Type> args, Type ret) {
    if (auto f = module.lookupSymbol<cir::FuncOp>(name)) return f;
    MLIRContext *ctx = &getContext();
    OpBuilder b = OpBuilder::atBlockEnd(module.getBody());
    auto f = cir::FuncOp::create(b, module.getLoc(), name,
                                 cir::FuncType::get(llvm::SmallVector<Type>(args.begin(), args.end()),
                                                    ret ? ret : cir::VoidType::get(ctx)));
    SymbolTable::setSymbolVisibility(f, SymbolTable::Visibility::Private);
    return f;
}

// The runtime's entry points take the queue as a void *; the analyses read
// a queue's type from the pointer its operations are handed. Each struct
// therefore calls them through functions of its own, which take a pointer
// to the struct, carry the annotations the lifting pass raises, and pass
// the pointer on: the queue's type is the struct, not void, so a void *
// the program hands elsewhere is not taken for one of its queues.
cir::FuncOp LiftCQueuesPass::thunk(Candidate &c, cir::FuncOp rt, llvm::ArrayRef<StringRef> ann) {
    MLIRContext *ctx = &getContext();
    std::string recName = c.rec.getName() ? c.rec.getName().getValue().str() : "anon";
    const std::string name = rt.getSymName().str() + "." + recName;
    if (auto f = module.lookupSymbol<cir::FuncOp>(name)) return f;
    llvm::SmallVector<Type> args(rt.getArgumentTypes().begin(), rt.getArgumentTypes().end());
    args[0] = cir::PointerType::get(c.rec);
    auto rets = rt.getFunctionType().getReturnTypes();
    OpBuilder b = OpBuilder::atBlockEnd(module.getBody());
    Location loc = module.getLoc();
    auto f = cir::FuncOp::create(b, loc, name,
                                 cir::FuncType::get(args, rets.empty() ? cir::VoidType::get(ctx) : rets.front()),
                                 cir::GlobalLinkageKind::InternalLinkage);
    SymbolTable::setSymbolVisibility(f, SymbolTable::Visibility::Private);
    llvm::SmallVector<Attribute, 4> as;
    for (StringRef a : ann) as.push_back(cir::AnnotationAttr::get(ctx, StringAttr::get(ctx, a), ArrayAttr()));
    f.setAnnotationsAttr(ArrayAttr::get(ctx, as));
    Block *entry = f.addEntryBlock();
    OpBuilder fb = OpBuilder::atBlockBegin(entry);
    llvm::SmallVector<Value> pass(entry->getArguments().begin(), entry->getArguments().end());
    pass[0] = cir::CastOp::create(fb, loc, rt.getArgumentTypes()[0], cir::CastKind::bitcast, pass[0]);
    auto call = cir::CallOp::create(fb, loc, FlatSymbolRefAttr::get(rt.getSymNameAttr()),
                                    rets.empty() ? Type() : rets.front(), pass);
    if (rets.empty()) cir::ReturnOp::create(fb, loc, ValueRange{});
    else cir::ReturnOp::create(fb, loc, ValueRange{call.getResult()});
    return f;
}

void LiftCQueuesPass::lift(Candidate &c) {
    MLIRContext *ctx = &getContext();
    auto voidPtr = cir::PointerType::get(cir::VoidType::get(ctx));
    auto s32 = cir::IntType::get(ctx, 32, true);
    auto s64 = cir::IntType::get(ctx, 64, true);
    auto u64 = cir::IntType::get(ctx, 64, false);
    auto s32p = cir::PointerType::get(s32);
    auto dataPtr = cir::PointerType::get(cir::PointerType::get(voidPtr));
    // The runtime's entry points and the struct's own functions over them,
    // only those its operations use.
    auto rInit = [&] {
        return thunk(c, runtime("wtc_c_queue_init", {voidPtr, s32, s64, s32, s32}, Type()),
                     {"wtc_queue_construction", "wtc_lifted_c_init"});
    };
    auto rDestroy = [&] { return thunk(c, runtime("wtc_c_queue_destroy", {voidPtr}, Type()), {"wtc_lifted_c_destroy"}); };
    auto rPush = [&] {
        return thunk(c, runtime("wtc_c_queue_push_wait", {voidPtr, voidPtr}, Type()),
                     {"wtc_queue_push_wait", "wtc_queue_bounded", "wtc_lifted_c_push"});
    };
    auto rPop = [&] {
        return thunk(c, runtime("wtc_c_queue_pop_wait", {voidPtr, cir::PointerType::get(voidPtr)}, s32),
                     {"wtc_queue_pop_wait", "wtc_queue_bounded", "wtc_lifted_c_pop"});
    };
    auto rTerm = [&] { return thunk(c, runtime("wtc_c_queue_terminate", {voidPtr}, Type()), {"wtc_lifted_c_terminate"}); };
    auto rPushB = [&] {
        return thunk(c, runtime("wtc_c_queue_push_batch", {voidPtr, dataPtr, s32p, s32p, u64, u64}, s32),
                     {"wtc_queue_push_batch", "wtc_queue_bounded", "wtc_lifted_c_push_batch"});
    };
    auto rPopB = [&] {
        return thunk(c, runtime("wtc_c_queue_pop_batch", {voidPtr, dataPtr, s32p, s32p, u64, u64}, s32),
                     {"wtc_queue_pop_batch", "wtc_queue_bounded", "wtc_lifted_c_pop_batch"});
    };
    const std::string recName = c.rec.getName() ? c.rec.getName().getValue().str() : "";

    for (Fn &fn : c.fns) {
        if (fn.role == Role::Helper) continue;
        cir::FuncOp f = fn.f;
        Location loc = f.getLoc();
        Region &body = f.getBody();
        body.dropAllReferences();
        body.getBlocks().clear();
        Block *entry = new Block();
        body.push_back(entry);
        for (Type t : f.getArgumentTypes()) entry->addArgument(t, loc);
        OpBuilder b = OpBuilder::atBlockBegin(entry);
        auto castTo = [&](Value v, Type t) -> Value {
            if (v.getType() == t) return v;
            if (isa<cir::PointerType>(v.getType()) && isa<cir::PointerType>(t))
                return cir::CastOp::create(b, loc, t, cir::CastKind::bitcast, v);
            return cir::CastOp::create(b, loc, t, cir::CastKind::integral, v);
        };
        auto callRt = [&](cir::FuncOp rt, llvm::ArrayRef<Value> args) -> Value {
            auto r = rt.getFunctionType().getReturnTypes();
            auto op = cir::CallOp::create(b, loc, FlatSymbolRefAttr::get(rt.getSymNameAttr()),
                                          r.empty() ? Type() : r.front(), args);
            return r.empty() ? Value() : op.getResult();
        };
        auto constant = [&](Type t, int64_t v) -> Value {
            return cir::ConstantOp::create(b, loc, t, cir::IntAttr::get(t, v));
        };
        Type retTy = f.getFunctionType().getReturnTypes().empty() ? Type() : f.getFunctionType().getReturnTypes().front();
        auto ret = [&](Value v) {
            if (v && retTy) cir::ReturnOp::create(b, loc, ValueRange{castTo(v, retTy)});
            else if (retTy) cir::ReturnOp::create(b, loc, ValueRange{constant(retTy, 0)});
            else cir::ReturnOp::create(b, loc, ValueRange{});
        };
        Value at = entry->getArgument(fn.q);
        // A member of the caller's ring, by its path.
        auto memberOf = [&](Value base, const std::vector<unsigned> &path) -> Value {
            Value v = base;
            for (unsigned i : path) {
                auto rec = cast<cir::RecordType>(cast<cir::PointerType>(v.getType()).getPointee());
                auto it = names.find({rec, i});
                v = cir::GetMemberOp::create(b, loc, cir::PointerType::get(rec.getMembers()[i]), v,
                                             it != names.end() ? it->second : std::to_string(i), i);
            }
            return v;
        };
        switch (fn.role) {
        case Role::Init: {
            // The queue is built where the program initialises it, at each
            // call, so that each queue has a construction of its own whose
            // tag the specialisation pass can choose; the body stays as a
            // fallback that is never called.
            auto build = [&](ValueRange args) {
                Value cap = castTo(args[c.capParam], s64);
                if (c.capOffset) cap = cir::AddOp::create(b, loc, s64, cap, constant(s64, c.capOffset));
                Value prods = c.counted ? castTo(args[c.prodParam], s32) : constant(s32, 0);
                // How the queue finishes (shim/c_queue.h): never, at exactly
                // `producers` terminations (a test for equality), or at as
                // many or more.
                const int64_t ends = !c.counted ? 0 : c.endEq ? 1 : 2;
                callRt(rInit(), {args[fn.q], constant(s32, 0), cap, prods, constant(s32, ends)});
            };
            build(entry->getArguments());
            ret(fn.constRet && retTy ? constant(retTy, *fn.constRet) : Value());
            for (cir::CallOp site : fn.calls) {
                OpBuilder sb(site);
                std::swap(b, sb);
                Location here = loc;
                loc = site.getLoc();
                build(site.getArgOperands());
                // The value init returned, a constant (checked).
                if (site.getNumResults() == 1)
                    site.getResult().replaceAllUsesWith(
                        constant(site.getResult().getType(), fn.constRet.value_or(0)));
                site.erase();
                loc = here;
                std::swap(b, sb);
            }
            break;
        }
        case Role::Destroy:
            callRt(rDestroy(), {at});
            ret(fn.constRet && retTy ? constant(retTy, *fn.constRet) : Value());
            break;
        case Role::Push:
            callRt(rPush(), {at, castTo(entry->getArgument(fn.valueParam), voidPtr)});
            ret(fn.pushRet && retTy ? constant(retTy, *fn.pushRet) : Value());
            break;
        case Role::Pop: {
            Value got = callRt(rPop(), {at, castTo(entry->getArgument(fn.outParam), cir::PointerType::get(voidPtr))});
            Value ok = cir::CmpOp::create(b, loc, cir::CmpOpKind::ne, got, constant(s32, 0));
            Value r = cir::SelectOp::create(b, loc, ok, constant(retTy, fn.takeRet->k),
                                            constant(retTy, fn.closedRet ? fn.closedRet->k : 0));
            ret(r);
            break;
        }
        case Role::Terminate:
            callRt(rTerm(), {at});
            ret(fn.constRet && retTy ? constant(retTy, *fn.constRet) : Value());
            break;
        case Role::PushBatch:
        case Role::PopBatch: {
            Value ring = entry->getArgument(fn.ringParam);
            Value data = castTo(memberOf(ring, fn.ring.d), dataPtr);
            Value in = castTo(memberOf(ring, fn.ring.in), s32p);
            Value out = castTo(memberOf(ring, fn.ring.out), s32p);
            Value slotsAddr = memberOf(ring, fn.ring.s);
            Value slots = cir::LoadOp::create(b, loc, cast<cir::PointerType>(slotsAddr.getType()).getPointee(), slotsAddr);
            slots = castTo(slots, u64);
            // The limit as a count: a signed one below zero is none.
            Value limit = entry->getArgument(fn.limitParam);
            if (auto it = cast<cir::IntType>(limit.getType()); it.isSigned()) {
                Value neg = cir::CmpOp::create(b, loc, cir::CmpOpKind::lt, limit, constant(it, 0));
                limit = cir::SelectOp::create(b, loc, neg, constant(it, 0), limit);
            }
            limit = castTo(limit, u64);
            Value got = callRt(fn.role == Role::PushBatch ? rPushB() : rPopB(), {at, data, in, out, slots, limit});
            if (fn.role == Role::PopBatch && fn.closedRet && !isK(fn.closedRet, -1)) {
                Value closed = cir::CmpOp::create(b, loc, cir::CmpOpKind::lt, got, constant(s32, 0));
                got = cir::SelectOp::create(b, loc, closed, constant(s32, fn.closedRet->k), got);
            }
            ret(got);
            break;
        }
        default:
            break;
        }
        f->setAttr("wtc.lifted_c", StringAttr::get(ctx, recName));
    }
    // Helpers nothing calls any more, if private.
    for (Fn &fn : c.fns)
        if (fn.role == Role::Helper && SymbolTable::symbolKnownUseEmpty(fn.f, module) && fn.f.isPrivate()) fn.f.erase();
    InFlightDiagnostic d = emitRemark(c.fns.front().f.getLoc()) << "wtc: C queue " << c.label << " lifted (";
    bool first = true;
    for (Fn &fn : c.fns) {
        if (fn.role == Role::Helper) continue;
        d << (first ? "" : ", ") << fn.f.getSymName() << ": " << roleName(fn.role);
        first = false;
    }
    d << "); capacity: parameter " << c.capParam << (c.capOffset >= 0 ? " + " : " - ")
      << (c.capOffset >= 0 ? c.capOffset : -c.capOffset)
      << (c.counted ? "; finished after as many terminations as parameter " + std::to_string(c.prodParam) + " of init"
                    : std::string());
}

} // namespace

namespace mlir::wtc {
void registerLiftCQueuesPass() {
    PassRegistration<LiftCQueuesPass>();
}
} // namespace mlir::wtc
