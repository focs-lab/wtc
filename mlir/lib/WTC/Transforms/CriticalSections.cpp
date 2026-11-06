#include "WTC/Transforms/CriticalSections.h"
#include "WTC/Transforms/LiftNames.h"
#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"

#include "mlir/IR/SymbolTable.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using namespace mlir;
using namespace mlir::wtc;

std::string MemberPath::str() const {
    std::string s;
    if (!global.empty()) s = "@" + global;
    else if (auto a = dyn_cast<BlockArgument>(root)) s = "arg" + std::to_string(a.getArgNumber());
    else s = "?";
    for (unsigned i : path) s += "." + std::to_string(i);
    return s;
}

std::optional<MemberPath> mlir::wtc::memberPathOf(Value addr) {
    llvm::SmallVector<unsigned, 4> rev;
    Value v = addr;
    for (unsigned guard = 0; guard < 64; ++guard) {
        v = traceToLocalRoot(v);
        if (auto gm = v.getDefiningOp<cir::GetMemberOp>()) {
            rev.push_back(gm.getIndex());
            v = gm.getAddr();
            continue;
        }
        if (Value r = traceToRoot(v); r != v) {
            v = r;
            continue;
        }
        MemberPath p;
        p.path.assign(rev.rbegin(), rev.rend());
        if (auto a = dyn_cast<BlockArgument>(v)) {
            auto f = dyn_cast<cir::FuncOp>(a.getOwner()->getParentOp());
            if (!f || a.getOwner() != &f.getBody().front()) return std::nullopt;
            p.root = v;
            return p;
        }
        if (auto g = v.getDefiningOp<cir::GetGlobalOp>()) {
            p.root = v;
            p.global = g.getName().str();
            return p;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool CriticalSection::contains(Operation *op) const {
    for (Operation *p = op; p; p = p->getParentOp())
        if (inside.contains(p)) return true;
    return false;
}

namespace {

bool isLockObjectCtor(cir::FuncOp callee) {
    Name n = nameOf(callee.getSymName());
    StringRef ctx = n.context;
    return n.ctor && (ctx.starts_with("std::lock_guard<") || ctx.starts_with("std::unique_lock<") ||
                      ctx.starts_with("std::scoped_lock<"));
}

// What a function itself reads, writes and hands to calls through its
// parameters: argument index, the path below it, and the kind.
struct ParamAccess {
    unsigned arg;
    llvm::SmallVector<unsigned, 4> path;
    enum Kind { Read, Write, Passed } kind;
};

class SectionBuilder {
public:
    SectionBuilder(cir::FuncOp f, std::vector<CriticalSection> &out,
            llvm::DenseMap<Operation *, std::vector<ParamAccess>> &accessCache)
        : f(f), out(out), accessCache(accessCache) {}

    void run() {
        std::vector<unsigned> held;
        walkRegion(f.getBody(), held);
    }

private:
    cir::FuncOp f;
    std::vector<CriticalSection> &out;
    llvm::DenseMap<Operation *, std::vector<ParamAccess>> &accessCache;
    llvm::DenseMap<unsigned, Value> lockSlot;  // section -> the lock object's slot

    // The held sections at the end of each block of the region that falls
    // through, intersected; nullopt when none does.
    std::optional<std::vector<unsigned>> walkRegion(Region &r, std::vector<unsigned> held) {
        if (r.empty()) return held;
        std::optional<std::vector<unsigned>> result;
        for (Block &b : r) {
            std::vector<unsigned> h = held;
            if (!walkBlock(b, h)) continue;
            if (!result) result = h;
            else
                llvm::erase_if(*result, [&](unsigned s) { return !llvm::is_contained(h, s); });
        }
        return result;
    }

    // Walks a block; false when it does not fall through.
    bool walkBlock(Block &b, std::vector<unsigned> &held) {
        for (Operation &op : b) {
            for (unsigned s : held) out[s].inside.insert(&op);
            if (isa<cir::ReturnOp, cir::UnreachableOp, cir::TrapOp>(op)) return false;
            if (auto call = dyn_cast<cir::CallOp>(op)) {
                callOp(call, held);
                continue;
            }
            access(&op, held);
            if (auto cl = dyn_cast<cir::CleanupScopeOp>(op)) {
                auto body = walkRegion(cl.getBodyRegion(), held);
                std::vector<unsigned> after = body ? *body : held;
                // The cleanup destroys the lock objects whose sections end
                // with the scope.
                Region &cr = cl.getCleanupRegion();
                if (!cr.empty())
                    cr.walk([&](cir::CallOp c) {
                        if (c.getNumOperands() != 1) return;
                        Value slot = traceToLocalRoot(c.getOperand(0));
                        llvm::erase_if(after, [&](unsigned s) { return lockSlot.lookup(s) == slot; });
                    });
                held = after;
                continue;
            }
            if (op.getNumRegions() == 0) continue;
            std::optional<std::vector<unsigned>> joined;
            for (Region &r : op.getRegions()) {
                auto h = walkRegion(r, held);
                if (!h) continue;
                if (!joined) joined = *h;
                else
                    llvm::erase_if(*joined, [&](unsigned s) { return !llvm::is_contained(*h, s); });
            }
            if (joined) held = *joined;
        }
        return true;
    }

    void access(Operation *op, const std::vector<unsigned> &held) {
        if (held.empty()) return;
        llvm::SmallVector<MemoryAccess, 2> acc;
        memoryAccesses(op, acc);
        for (const MemoryAccess &a : acc) {
            auto p = memberPathOf(a.addr);
            if (!p) continue;
            for (unsigned s : held) (a.isWrite ? out[s].writes : out[s].reads).insert(p->str());
        }
    }

    void callOp(cir::CallOp call, std::vector<unsigned> &held) {
        cir::FuncOp callee = lookupCallee(call);
        if (callee && isLockObjectCtor(callee) && call.getNumOperands() == 2) {
            if (auto m = memberPathOf(call.getOperand(1))) {
                open(call, *m, held);
                lockSlot[held.back()] = traceToLocalRoot(call.getOperand(0));
            }
            return;
        }
        if (callee && callee.getSymName() == "pthread_mutex_lock" && call.getNumOperands() == 1) {
            if (auto m = memberPathOf(call.getOperand(0))) open(call, *m, held);
            return;
        }
        if (callee && callee.getSymName() == "pthread_mutex_unlock" && call.getNumOperands() == 1) {
            if (auto m = memberPathOf(call.getOperand(0)))
                for (auto it = held.rbegin(); it != held.rend(); ++it)
                    if (out[*it].mutex == *m && !lockSlot.count(*it)) {
                        if (call->getBlock() == out[*it].lock->getBlock()) out[*it].unlock = call;
                        held.erase(std::next(it).base());
                        break;
                    }
            return;
        }
        // A lock object's own unlock() and destructor end its section.
        if (call.getNumOperands() >= 1) {
            Value slot = traceToLocalRoot(call.getOperand(0));
            for (auto it = held.begin(); it != held.end(); ++it)
                if (lockSlot.count(*it) && lockSlot.lookup(*it) == slot) {
                    if (callee && (nameOf(callee.getSymName()).base == "unlock" || nameOf(callee.getSymName()).dtor)) {
                        if (call->getBlock() == out[*it].lock->getBlock()) out[*it].unlock = call;
                        held.erase(it);
                    }
                    return;
                }
        }
        if (held.empty()) return;
        for (unsigned s : held) out[s].calls.push_back(call);
        // What the call is handed, and, one level down, what the callee
        // itself reads and writes through it.
        llvm::SmallVector<std::optional<MemberPath>, 4> args;
        for (Value a : call.getArgOperands()) {
            args.push_back(memberPathOf(a));
            if (args.back())
                for (unsigned s : held) out[s].reads.insert(args.back()->str());
        }
        if (!callee || callee.getBody().empty()) return;
        for (const ParamAccess &pa : paramAccesses(callee)) {
            if (pa.arg >= args.size() || !args[pa.arg]) continue;
            MemberPath p = *args[pa.arg];
            p.path.append(pa.path.begin(), pa.path.end());
            for (unsigned s : held) {
                if (pa.kind == ParamAccess::Write) out[s].writes.insert(p.str());
                else out[s].reads.insert(p.str());
            }
        }
    }

    void open(cir::CallOp call, const MemberPath &m, std::vector<unsigned> &held) {
        CriticalSection cs;
        cs.f = f;
        cs.lock = call;
        cs.mutex = m;
        out.push_back(std::move(cs));
        held.push_back(out.size() - 1);
    }

    const std::vector<ParamAccess> &paramAccesses(cir::FuncOp g) {
        auto [it, fresh] = accessCache.try_emplace(g.getOperation());
        if (!fresh) return it->second;
        std::vector<ParamAccess> acc;
        auto add = [&](Value addr, ParamAccess::Kind k) {
            auto p = memberPathOf(addr);
            if (!p || !p->global.empty()) return;
            auto a = cast<BlockArgument>(p->root);
            if (a.getOwner()->getParentOp() != g.getOperation()) return;
            acc.push_back(ParamAccess{a.getArgNumber(), p->path, k});
        };
        g.walk([&](Operation *op) {
            if (auto c = dyn_cast<cir::CallOp>(op)) {
                for (Value a : c.getArgOperands()) add(a, ParamAccess::Passed);
                return;
            }
            llvm::SmallVector<MemoryAccess, 2> m;
            memoryAccesses(op, m);
            for (const MemoryAccess &x : m) add(x.addr, x.isWrite ? ParamAccess::Write : ParamAccess::Read);
        });
        return accessCache[g.getOperation()] = std::move(acc);
    }
};

}  // namespace

CriticalSections::CriticalSections(ModuleOp m) : module(m) {
    llvm::DenseMap<Operation *, std::vector<ParamAccess>> cache;
    for (cir::FuncOp f : module.getOps<cir::FuncOp>()) {
        if (f.getBody().empty()) continue;
        std::vector<CriticalSection> found;
        SectionBuilder(f, found, cache).run();
        if (!found.empty()) sections[f.getOperation()] = std::move(found);
    }
}

llvm::ArrayRef<CriticalSection> CriticalSections::in(cir::FuncOp f) const {
    auto it = sections.find(f.getOperation());
    return it == sections.end() ? llvm::ArrayRef<CriticalSection>() : llvm::ArrayRef<CriticalSection>(it->second);
}

const CriticalSection *CriticalSections::enclosing(Operation *op) const {
    auto f = op->getParentOfType<cir::FuncOp>();
    if (!f) return nullptr;
    const CriticalSection *best = nullptr;
    for (const CriticalSection &cs : in(f))
        if (cs.contains(op)) best = &cs;
    return best;
}

llvm::SmallVector<cir::CallOp, 2> CriticalSections::callsUnderLock(cir::FuncOp f, unsigned self,
                                                                     unsigned mutexMember) const {
    llvm::SmallVector<cir::CallOp, 2> calls;
    auto uses = SymbolTable::getSymbolUses(f, module);
    if (!uses) return {};
    for (const SymbolTable::SymbolUse &u : *uses) {
        auto call = dyn_cast<cir::CallOp>(u.getUser());
        if (!call || call.getCalleeAttr() != u.getSymbolRef() || call.getNumOperands() <= self) return {};
        auto obj = memberPathOf(call.getOperand(self));
        if (!obj) return {};
        MemberPath want = *obj;
        want.path.push_back(mutexMember);
        auto caller = call->getParentOfType<cir::FuncOp>();
        bool under = false;
        for (const CriticalSection &cs : in(caller))
            under |= cs.contains(call) && cs.mutex == want;
        if (!under) return {};
        calls.push_back(call);
    }
    return calls;
}

namespace {

// Prints every critical section as a remark, for the tests.
class CriticalSectionsPass : public PassWrapper<CriticalSectionsPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CriticalSectionsPass)
    StringRef getArgument() const final { return "wtc-critical-sections"; }
    StringRef getDescription() const final {
        return "Report every critical section: its mutex, the member paths read and written with it held, and "
               "the calls made there";
    }
    void runOnOperation() override {
        ModuleOp module = getOperation();
        CriticalSections all(module);
        for (cir::FuncOp f : module.getOps<cir::FuncOp>())
            for (const CriticalSection &cs : all.in(f)) {
                InFlightDiagnostic d = emitRemark(cs.lock->getLoc())
                                       << "wtc: critical section in " << pretty(f.getSymName()) << " on "
                                       << cs.mutex.str() << (cs.unlock ? ", unlocked in its block" : "");
                d << "; reads " << llvm::join(cs.reads, " ") << "; writes " << llvm::join(cs.writes, " ")
                  << "; calls";
                for (cir::CallOp c : cs.calls)
                    if (cir::FuncOp g = lookupCallee(c)) d << " " << pretty(g.getSymName());
            }
    }
};

}  // namespace

namespace mlir::wtc {
void registerWTCCriticalSectionsPass() { PassRegistration<CriticalSectionsPass>(); }
}  // namespace mlir::wtc
