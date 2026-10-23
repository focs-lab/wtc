#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Demangle/Demangle.h"

#include <cstdlib>
#include <optional>
#include <string>

using namespace mlir;
using namespace mlir::wtc;

// Does the consumer of a queue only borrow what it pops?
//
// The shim's try_pop returns a shared_ptr. An implementation that holds the
// elements inline in the ring (topology::spsc_inline) hands the element out
// in place, through a shared_ptr that owns nothing, and destroys it at the
// consumer's next pop. That is only correct if the program never notices
// the difference, which is what this pass proves for every pop of a queue
// already proved spsc, or proved spsc when the loops that start its threads
// run at most once (Utils.h, spscWhenLoopsRunOnce). For each pop, whose
// shared_ptr the compiler builds in a stack slot (the hidden return slot of
// try_pop):
//
//   (a) the slot is used only by the pop, its lifetime markers, and members
//       of shared_ptr that neither copy nor share it: operator bool, get,
//       operator*, operator->, comparison with nullptr, the destructor.
//       Anything else -- a copy, a move, reset, use_count, passing it on --
//       could keep the element alive or observe that nothing owns it.
//   (b) a pointer into the element, from get, * or ->, is only read
//       through, written through, stepped into a member or element,
//       compared, kept in a local slot whose address goes nowhere, pushed
//       on to a queue (which copies it), or passed to a function with a body
//       that treats its parameter the same way.
//       It is never stored anywhere else, returned, or turned into an
//       integer, so nothing can reach the element after the slot is gone.
//   (c) while the slot lives, nothing in its scope pops the same queue: no
//       other pop that may act on it -- its own, one of several queues it
//       is among, or one of its type the analysis could not place -- and no
//       call that may reach one.
//
// The slot is a local of the scope that holds it, so the scope ends, and
// with it the borrow, before any later pop of the same queue that is not
// inside that scope. A queue is borrowed only if every one of its pops
// passes; the root then gets wtc.borrowed and each pop `borrowed`. A remark
// names the first use that stands in the way otherwise. A pop that may act
// on several queues (a helper every stage calls with its own) is one of
// each one's pops and is checked for each: what it does with its element
// does not depend on which queue it came from.
//
// Assumes one translation unit, as the ownership analysis does: a function
// without a body here cannot pop a queue this module defines.

namespace {

enum class SharedPtrMember { None, Bool, Get, Deref, Arrow, Dtor, CompareNull };

// What a call into the standard library's shared_ptr is, by its demangled
// name rather than its mangling, which differs between libstdc++ and libc++
// and between versions of each (inline namespaces, ABI tags).
SharedPtrMember classifySharedPtrCall(cir::FuncOp f) {
    if (!f) return SharedPtrMember::None;
    const std::string mangled = f.getSymName().str();
    llvm::ItaniumPartialDemangler d;
    if (d.partialDemangle(mangled.c_str())) return SharedPtrMember::None;
    auto take = [](char *s) {
        std::string r = s ? s : "";
        std::free(s);
        return r;
    };
    std::string ctx = take(d.getFunctionDeclContextName(nullptr, nullptr));
    std::string base = take(d.getFunctionBaseName(nullptr, nullptr));
    std::string params = take(d.getFunctionParameters(nullptr, nullptr));
    if (auto lt = ctx.find('<'); lt != std::string::npos) ctx.resize(lt);
    if (auto br = base.find('['); br != std::string::npos) base.resize(br);  // [abi:...]
    if (auto lt = base.find('<'); lt != std::string::npos && base.rfind("operator", 0) != 0)
        base.resize(lt);

    const bool sharedPtr = ctx == "std::shared_ptr" || ctx == "std::__1::shared_ptr";
    const bool member = sharedPtr || ctx == "std::__shared_ptr" || ctx == "std::__shared_ptr_access";
    if (member) {
        if (d.isCtorOrDtor() && !base.empty() && base[0] == '~') return SharedPtrMember::Dtor;
        if (base == "operator bool") return SharedPtrMember::Bool;
        if (base == "get") return SharedPtrMember::Get;
        if (base == "operator*") return SharedPtrMember::Deref;
        if (base == "operator->") return SharedPtrMember::Arrow;
        return SharedPtrMember::None;
    }
    if ((ctx == "std" || ctx == "std::__1") && (base == "operator==" || base == "operator!=") &&
        params.find("shared_ptr") != std::string::npos &&
        (params.find("decltype(nullptr)") != std::string::npos ||
         params.find("nullptr_t") != std::string::npos))
        return SharedPtrMember::CompareNull;
    return SharedPtrMember::None;
}

struct Refusal {
    Operation *at;
    std::string why;
};

std::string calleeName(cir::CallOp call) {
    cir::FuncOp f = lookupCallee(call);
    if (!f) return "an indirect callee";
    return llvm::demangle(f.getSymName().str());
}

class BorrowChecker {
public:
    explicit BorrowChecker(ModuleOp module) : module(module) {}

    std::optional<Refusal> checkPop(QueuePopOp pop, int64_t id, Type queueType);

private:
    ModuleOp module;
    // (function, parameter) -> does that function only borrow through it?
    // Seeded optimistically on entry, so recursion through the same
    // parameter adds nothing; every other use is still checked.
    llvm::DenseMap<std::pair<Operation *, unsigned>, bool> paramMemo;
    // function -> may it (transitively) pop the queue being checked?
    llvm::DenseMap<Operation *, bool> popMemo;
    int64_t curId = -1;
    Type curType;

    std::optional<Refusal> checkSlotUses(Value v, QueuePopOp pop);
    std::optional<Refusal> checkPointer(Value p, unsigned depth);
    bool paramOnlyBorrows(cir::FuncOp f, unsigned idx, unsigned depth);
    bool popsSameQueue(QueuePopOp other) const;
    bool mayPop(cir::FuncOp f);
    std::optional<Refusal> checkScope(Region *scope, QueuePopOp pop);
};

bool BorrowChecker::popsSameQueue(QueuePopOp other) const {
    if (auto id = other.getQueueIdAttr()) return id.getInt() == curId;
    if (auto ids = other->getAttrOfType<DenseI64ArrayAttr>(kQueueIdsAttrName))
        return llvm::is_contained(ids.asArrayRef(), curId);
    // A pop the analysis could not place names no queue; it may act on
    // this one if it acts on a queue of the same type.
    auto qt = dyn_cast<QueueType>(other.getQueue().getType());
    return !qt || qt.getElementType() == curType;
}

bool BorrowChecker::mayPop(cir::FuncOp f) {
    if (!f || f.getBody().empty()) return false;  // one translation unit
    if (auto it = popMemo.find(f); it != popMemo.end()) return it->second;
    popMemo[f] = false;  // a cycle adds nothing the other paths do not
    bool result = false;
    f.walk([&](Operation *op) {
        if (result) return WalkResult::interrupt();
        if (auto pop = dyn_cast<QueuePopOp>(op)) {
            if (popsSameQueue(pop)) result = true;
        } else if (auto call = dyn_cast<cir::CallOp>(op)) {
            if (cir::FuncOp callee = lookupCallee(call)) {
                if (mayPop(callee)) result = true;
            } else {
                auto targets = indirectCallees(call);
                if (targets.empty()) result = true;  // could be anything
                for (cir::FuncOp t : targets)
                    if (mayPop(t)) result = true;
            }
        }
        return result ? WalkResult::interrupt() : WalkResult::advance();
    });
    popMemo[f] = result;
    return result;
}

std::optional<Refusal> BorrowChecker::checkScope(Region *scope, QueuePopOp pop) {
    std::optional<Refusal> out;
    scope->walk([&](Operation *op) {
        if (out) return WalkResult::interrupt();
        if (auto other = dyn_cast<QueuePopOp>(op)) {
            if (other != pop && popsSameQueue(other))
                out = Refusal{op, "the same queue is popped again while the element is borrowed"};
        } else if (auto call = dyn_cast<cir::CallOp>(op)) {
            cir::FuncOp callee = lookupCallee(call);
            if (callee && classifySharedPtrCall(callee) != SharedPtrMember::None)
                return WalkResult::advance();
            bool may = false;
            if (callee) {
                may = mayPop(callee);
            } else {
                auto targets = indirectCallees(call);
                may = targets.empty();
                for (cir::FuncOp t : targets) may |= mayPop(t);
            }
            if (may)
                out = Refusal{op, "a call to " + calleeName(call) +
                                      " may pop the same queue while the element is borrowed"};
        }
        return out ? WalkResult::interrupt() : WalkResult::advance();
    });
    return out;
}

std::optional<Refusal> BorrowChecker::checkSlotUses(Value v, QueuePopOp pop) {
    for (OpOperand &use : v.getUses()) {
        Operation *user = use.getOwner();
        if (user == pop.getOperation()) continue;
        if (isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(user)) continue;
        if (auto up = dyn_cast<cir::BaseClassAddrOp>(user)) {
            if (auto r = checkSlotUses(up.getResult(), pop)) return r;
            continue;
        }
        if (auto cast = dyn_cast<cir::CastOp>(user); cast && cast.getKind() == cir::CastKind::bitcast) {
            if (auto r = checkSlotUses(cast.getResult(), pop)) return r;
            continue;
        }
        if (auto call = dyn_cast<cir::CallOp>(user)) {
            switch (classifySharedPtrCall(lookupCallee(call))) {
            case SharedPtrMember::Bool:
            case SharedPtrMember::Dtor:
            case SharedPtrMember::CompareNull:
                continue;
            case SharedPtrMember::Get:
            case SharedPtrMember::Deref:
            case SharedPtrMember::Arrow:
                if (call.getNumResults() != 1)
                    return Refusal{user, "an access to the element returns nothing to follow"};
                if (auto r = checkPointer(call.getResult(), 0)) return r;
                continue;
            case SharedPtrMember::None:
                return Refusal{user, "the popped shared_ptr is passed to " + calleeName(call)};
            }
        }
        return Refusal{user, "the popped shared_ptr is used by " +
                                 user->getName().getStringRef().str()};
    }
    return std::nullopt;
}

std::optional<Refusal> BorrowChecker::checkPointer(Value p, unsigned depth) {
    if (depth > 8)
        return Refusal{p.getDefiningOp() ? p.getDefiningOp() : p.getParentBlock()->getParentOp(),
                       "a pointer into the element is followed through too many calls"};
    for (OpOperand &use : p.getUses()) {
        Operation *user = use.getOwner();
        if (isa<cir::LoadOp, cir::CmpOp>(user)) continue;
        if (auto copy = dyn_cast<cir::CopyOp>(user)) {
            (void)copy;  // reading from the element or writing into it
            continue;
        }
        if (auto st = dyn_cast<cir::StoreOp>(user)) {
            if (st.getValue() != p) continue;  // a write into the element
            // The pointer itself is stored. Fine only into a local slot whose
            // address goes nowhere: then every load of it is the pointer again.
            auto slot = st.getAddr().getDefiningOp<cir::AllocaOp>();
            if (!slot || addressEscapes(slot))
                return Refusal{user, "a pointer into the element is stored where it may outlive the borrow"};
            for (Operation *u : slot.getResult().getUsers())
                if (auto ld = dyn_cast<cir::LoadOp>(u))
                    if (auto r = checkPointer(ld.getResult(), depth)) return r;
            continue;
        }
        if (auto m = dyn_cast<cir::GetMemberOp>(user)) {
            if (auto r = checkPointer(m.getResult(), depth)) return r;
            continue;
        }
        if (auto e = dyn_cast<cir::GetElementOp>(user)) {
            if (use.getOperandNumber() != 0)
                return Refusal{user, "a pointer into the element is used as an index"};
            if (auto r = checkPointer(e.getResult(), depth)) return r;
            continue;
        }
        if (auto s = dyn_cast<cir::PtrStrideOp>(user)) {
            if (use.getOperandNumber() != 0)
                return Refusal{user, "a pointer into the element is used as a stride"};
            if (auto r = checkPointer(s.getResult(), depth)) return r;
            continue;
        }
        if (auto c = dyn_cast<cir::CastOp>(user)) {
            if (c.getKind() == cir::CastKind::bitcast || c.getKind() == cir::CastKind::array_to_ptrdecay) {
                if (auto r = checkPointer(c.getResult(), depth)) return r;
                continue;
            }
            if (c.getKind() == cir::CastKind::ptr_to_bool) continue;
            return Refusal{user, "a pointer into the element is converted to an integer"};
        }
        if (auto call = dyn_cast<cir::CallOp>(user)) {
            cir::FuncOp callee = lookupCallee(call);
            if (!callee)
                return Refusal{user, "a pointer into the element is passed to an indirect call"};
            if (callee.getBody().empty())
                return Refusal{user, "a pointer into the element is passed to " + calleeName(call) +
                                         ", whose body is not visible here"};
            if (!paramOnlyBorrows(callee, use.getOperandNumber(), depth + 1))
                return Refusal{user, "a pointer into the element is passed to " + calleeName(call) +
                                         ", which keeps it or passes it on"};
            continue;
        }
        // A push copies the value it is given (decisions/0003), so the
        // element pushed on is read, not kept.
        if (auto push = dyn_cast<QueuePushOp>(user); push && push.getValue() == p) continue;
        if (auto push = dyn_cast<QueuePushDeferredOp>(user); push && push.getValue() == p) continue;
        if (isa<cir::ReturnOp>(user))
            return Refusal{user, "a pointer into the element is returned"};
        return Refusal{user, "a pointer into the element is used by " +
                                 user->getName().getStringRef().str()};
    }
    return std::nullopt;
}

bool BorrowChecker::paramOnlyBorrows(cir::FuncOp f, unsigned idx, unsigned depth) {
    auto key = std::make_pair(f.getOperation(), idx);
    if (auto it = paramMemo.find(key); it != paramMemo.end()) return it->second;
    paramMemo[key] = true;
    Block &entry = f.getBody().front();
    bool ok = idx < entry.getNumArguments() && !checkPointer(entry.getArgument(idx), depth);
    paramMemo[key] = ok;
    return ok;
}

std::optional<Refusal> BorrowChecker::checkPop(QueuePopOp pop, int64_t id, Type queueType) {
    curId = id;
    curType = queueType;
    popMemo.clear();
    // A transfer moves the element into the program's own object and keeps
    // nothing of the queue's: it borrows nothing, so it stands in the way
    // of nothing.
    if (pop.getTransfer()) return std::nullopt;
    Value out = pop.getOut();
    if (!out) return Refusal{pop, "the popped value is not returned through a slot"};
    auto slot = out.getDefiningOp<cir::AllocaOp>();
    if (!slot) return Refusal{pop, "the popped value is written somewhere other than a local slot"};
    if (!slot->getParentRegion()->isAncestor(pop->getParentRegion()))
        return Refusal{pop, "the pop is outside the scope of its slot"};
    if (auto r = checkSlotUses(slot.getResult(), pop)) return r;
    return checkScope(slot->getParentRegion(), pop);
}

class WTCBorrowAnalysisPass
    : public PassWrapper<WTCBorrowAnalysisPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCBorrowAnalysisPass)

    StringRef getArgument() const final { return "wtc-borrow-analysis"; }
    StringRef getDescription() const final {
        return "Mark the spsc queues whose every pop only borrows its element "
               "(run after --queue-ownership-analysis)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        ModuleOp module = getOperation();
        MLIRContext *ctx = &getContext();

        llvm::DenseMap<int64_t, llvm::SmallVector<QueuePopOp, 4>> popsOf;
        llvm::SmallPtrSet<Type, 4> anonymousPopTypes;
        module.walk([&](QueuePopOp pop) {
            if (auto id = pop.getQueueIdAttr()) popsOf[id.getInt()].push_back(pop);
            else if (auto ids = pop->getAttrOfType<DenseI64ArrayAttr>(kQueueIdsAttrName))
                for (int64_t i : ids.asArrayRef()) popsOf[i].push_back(pop);
            else if (auto qt = dyn_cast<QueueType>(pop.getQueue().getType()))
                anonymousPopTypes.insert(qt.getElementType());
        });

        BorrowChecker checker(module);
        module.walk([&](Operation *root) {
            // A queue's root: the global or the slot that is the queue, or
            // the one constructor call that builds a member or heap queue.
            const bool built = isa<cir::CallOp>(root) && root->hasAttr(kTopologyAttrName);
            if (!isa<cir::GlobalOp, cir::AllocaOp>(root) && !built) return;
            auto id = root->getAttrOfType<IntegerAttr>(kQueueIdAttrName);
            auto topo = root->getAttrOfType<TopologyAttr>(kTopologyAttrName);
            // Spsc, or spsc when the loops that start its threads each run
            // at most once: whether a pop only borrows does not depend on
            // the count, and the inline ring is chosen only at a count of one.
            if (!id || !topo || !spscWhenLoopsRunOnce(root)) return;
            const std::string label = labelOf(root);
            // Asked first: a pop of its type that the analysis could not
            // place may be one of its pops, and "nothing pops it" would be
            // wrong.
            Type queueType;
            if (auto g = dyn_cast<cir::GlobalOp>(root)) queueType = g.getSymType();
            else if (auto a = dyn_cast<cir::AllocaOp>(root)) queueType = a.getAllocaType();
            else queueType = cast<cir::PointerType>(root->getOperand(0).getType()).getPointee();
            if (anonymousPopTypes.contains(queueType)) {
                root->emitRemark() << "wtc: queue " << label
                                   << ": not borrowed: a pop of its type that the analysis could not "
                                      "place was not checked";
                return;
            }
            auto it = popsOf.find(id.getInt());
            if (it == popsOf.end() || it->second.empty()) {
                root->emitRemark() << "wtc: queue " << label << ": not borrowed: nothing pops it";
                return;
            }
            for (QueuePopOp pop : it->second) {
                if (auto r = checker.checkPop(pop, id.getInt(), queueType)) {
                    InFlightDiagnostic d = root->emitRemark()
                                           << "wtc: queue " << label << ": not borrowed: " << r->why;
                    d.attachNote(r->at->getLoc()) << "here";
                    return;
                }
            }
            root->setAttr(kBorrowedAttrName, UnitAttr::get(ctx));
            for (QueuePopOp pop : it->second) pop.setBorrowed(true);
            const bool anyTransfer = llvm::any_of(it->second, [](QueuePopOp p) { return p.getTransfer(); });
            root->emitRemark() << "wtc: queue " << label
                               << (anyTransfer ? ": every pop moves its element out or only borrows it ("
                                               : ": every pop only borrows its element (")
                               << it->second.size() << (it->second.size() == 1 ? " pop)" : " pops)");
        });
        markAllAnalysesPreserved();
    }

private:
    static std::string labelOf(Operation *root) {
        if (auto g = dyn_cast<cir::GlobalOp>(root)) return ("@" + g.getSymName()).str();
        if (auto a = dyn_cast<cir::AllocaOp>(root)) return ("%" + a.getName()).str();
        if (auto n = root->getAttrOfType<StringAttr>(kQueueNameAttrName)) return n.str();
        return "<queue>";
    }
};

} // namespace

namespace mlir::wtc {
void registerWTCBorrowAnalysisPass() { PassRegistration<WTCBorrowAnalysisPass>(); }
} // namespace mlir::wtc
