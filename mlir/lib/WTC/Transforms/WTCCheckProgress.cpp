#include "WTC/Transforms/Passes.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCInterfaces.h"
#include "WTC/WTCOps.h"

#include "clang/CIR/Dialect/IR/CIRDialect.h"

#include "mlir/IR/BuiltinOps.h"
#include "mlir/Pass/Pass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/Regex.h"

#include <optional>
#include <string>

using namespace mlir;
using namespace mlir::wtc;

// Holds a function declared lock-free (WTC_LOCK_FREE, shim/progress.hpp)
// to its promise: nothing it can reach may block. That is a reachability
// question over the call graph, and the answer is far more use as a call
// chain than as a latency spike in production.
//
// What blocks (decisions/0004):
//   - a wtc operation whose implementation, as recorded on it, can wait
//     for another thread or may call the allocator (ProgressOpInterface);
//   - a call to a function without a body here that is known to block or
//     allocate: the allocator, the threading library's waits, input and
//     output, sleeping, throwing; and a throw itself;
//   - any other function without a body here, and any indirect call whose
//     targets are not known: what cannot be seen is counted as blocking;
//   - inline assembly, unless it is made only of instructions that cannot
//     wait (onlyNonBlockingInstructions below).
// A call to a function with a body is followed into it. A loop that spins
// on an atomic waiting for another thread is not judged: the check is a
// necessary condition for lock-freedom, not a proof of it.
//
// One exception, and it is what makes a consumer of an inline queue pass:
// the destructor of the shared_ptr a pop returns, when the pop is borrowed
// and its queue holds elements inline, frees nothing -- the shared_ptr
// owns nothing -- and is not followed.

namespace {

struct Finding {
    Operation *leaf;
    std::string why;
    llvm::SmallVector<Operation *, 8> chain;  // the calls from the function to the leaf
    llvm::SmallVector<cir::FuncOp, 8> via;    // the function each of them enters
};

bool startsWith(const std::string &s, llvm::StringRef p) { return llvm::StringRef(s).starts_with(p); }

// Why a function without a body here blocks, by its name. Known offenders
// get their reason; everything else gets the default, since a function
// that cannot be seen into cannot be cleared.
std::string externalReason(llvm::StringRef mangled) {
    const std::string name = llvm::demangle(mangled.str());
    static const llvm::StringSet<> alloc = {"malloc", "calloc", "realloc", "free", "aligned_alloc",
                                             "posix_memalign"};
    if (alloc.contains(mangled) || startsWith(name, "operator new") || startsWith(name, "operator delete"))
        return "calls the allocator (" + name + ")";
    if (mangled.starts_with("pthread_mutex_") || mangled.starts_with("pthread_cond_") ||
        mangled.starts_with("pthread_rwlock_") || mangled.starts_with("sem_") ||
        mangled == "pthread_join" || mangled == "syscall" || mangled.contains("futex"))
        return "waits in the threading library (" + name + ")";
    if (mangled == "printf" || mangled == "fprintf" || mangled == "puts" || mangled == "fputs" ||
        mangled == "write" || mangled == "read" || mangled == "fwrite" || mangled == "fflush" ||
        startsWith(name, "std::basic_ostream") || startsWith(name, "std::operator<<"))
        return "does input or output (" + name + ")";
    if (mangled == "sleep" || mangled == "usleep" || mangled == "nanosleep" || mangled == "sched_yield")
        return "sleeps or yields (" + name + ")";
    if (mangled.starts_with("__cxa_throw") || mangled.starts_with("__cxa_allocate_exception") ||
        mangled.starts_with("__cxa_guard_acquire"))
        return "throws or takes a static-initialisation lock (" + name + ")";
    return "calls " + name + ", whose body is not visible here, so it is counted as blocking";
}

// Inline assembly is not looked into, except for the handful of
// instructions a lock-free fast path is made of and none of which can wait:
// prefetches, the spin-wait hint, fences and the time-stamp counter. A
// string made only of those (DPDK's rte_prefetch0 is "prefetcht0 $0", its
// rte_pause "pause"), or of nothing at all (the compiler barrier
// asm volatile("" ::: "memory")), is cleared; anything else keeps the
// default. One locked instruction is among them: adding an immediate zero
// to a slot below the stack pointer, the full fence DPDK and others use
// instead of mfence ("lock addl $0, -128(%rsp)", spelled "$$0" in the
// template). It is one locked read-modify-write, with no loop, on a line
// no other thread holds, so it cannot wait.
//
// Statements end at a newline, a carriage return or a semicolon, as the
// assembler's lexer ends them; a string holding "%%" (a statement separator
// on some targets) or a control character other than those and a tab is
// not cleared. A prefix standing alone ("rep;", "lock;") belongs to the
// next statement and is read with it. Each allowed instruction is matched
// with its operands: the bare ones may be followed only by a comment, the
// others only by the operands they take. A comment is "//" anywhere, or
// "#" after a bare instruction: "#" starts a comment in x86 assembly and
// marks an immediate on AArch64, where of the bare instructions only isb
// takes one, and an isb with any option is still only a barrier.
bool onlyNonBlockingInstructions(llvm::StringRef text) {
    static const llvm::StringSet<> bare = {"pause",  "lfence", "mfence", "sfence", "rdtsc",
                                           "rdtscp", "nop",    "yield",  "isb"};
    static const llvm::Regex x86Prefetch(
        "^prefetch(t0|t1|t2|nta|w)?[ \t]+"
        "(\\$[0-9]+|\\$\\{[0-9]+(:[a-z]+)?\\}|-?[0-9]*\\(%[a-z0-9]+(,[ \t]*%[a-z0-9]+(,[ \t]*[1248])?)?\\))$");
    static const llvm::Regex armPrefetch(
        "^prfm[ \t]+p(ld|li|st)l[123](keep|strm),[ \t]*"
        "(\\[[a-z0-9]+(,[ \t]*#-?[0-9]+)?\\]|\\[\\$[0-9]+\\]|\\[\\$\\{[0-9]+(:[a-z]+)?\\}\\]"
        "|\\$[0-9]+|\\$\\{[0-9]+(:[a-z]+)?\\})$");
    static const llvm::Regex armBarrier(
        "^(dmb|dsb)[ \t]+(sy|ld|st|ish|ishld|ishst|osh|oshld|oshst|nsh|nshld|nshst)$");
    static const llvm::Regex stackFence("^lock[ \t]+add[bwlq]?[ \t]+\\$\\$0,[ \t]*-[0-9]+\\(%rsp\\)$");

    if (text.contains("%%")) return false;
    for (char c : text)
        if ((static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7f)
            return false;

    llvm::SmallVector<llvm::StringRef, 8> stmts;
    for (size_t start = 0, i = 0; i <= text.size(); ++i)
        if (i == text.size() || text[i] == '\n' || text[i] == '\r' || text[i] == ';') {
            stmts.push_back(text.slice(start, i));
            start = i + 1;
        }

    llvm::SmallString<64> lower;
    auto lowered = [&](llvm::StringRef stmt) -> llvm::StringRef {
        lower.clear();
        for (char c : stmt) lower.push_back(llvm::toLower(c));
        return lower.str();
    };
    // A bare instruction, optionally followed by a comment.
    auto bareAlone = [&](llvm::StringRef stmt, llvm::StringRef only = {}) {
        auto [mnemonic, rest] = stmt.split(' ');
        if (mnemonic.contains('\t')) std::tie(mnemonic, rest) = stmt.split('\t');
        rest = rest.trim();
        if (!bare.contains(mnemonic) || (!only.empty() && mnemonic != only)) return false;
        return rest.empty() || rest.starts_with("#");
    };
    llvm::SmallString<64> joined;
    for (size_t i = 0; i < stmts.size(); ++i) {
        llvm::StringRef stmt = lowered(stmts[i].trim());
        stmt = stmt.split("//").first.rtrim();
        if (stmt.empty() || stmt.starts_with("#")) continue;  // nothing, or a comment
        if ((stmt == "rep" || stmt == "lock") && i + 1 < stmts.size()) {
            joined = stmt;
            joined += ' ';
            joined += lowered(stmts[++i].trim());
            stmt = llvm::StringRef(joined).split("//").first.rtrim();
        }
        if (bareAlone(stmt)) continue;
        // "rep; nop" and "rep nop" are pause, spelled out.
        if (stmt.starts_with("rep ") || stmt.starts_with("rep\t")) {
            if (!bareAlone(stmt.drop_front(4).ltrim(), "nop")) return false;
            continue;
        }
        if (x86Prefetch.match(stmt) || armPrefetch.match(stmt) || armBarrier.match(stmt) ||
            stackFence.match(stmt))
            continue;
        return false;
    }
    return true;
}

class ProgressChecker {
public:
    explicit ProgressChecker(ModuleOp module) : module(module) { collectExemptions(); }

    std::optional<Finding> blockingIn(cir::FuncOp f);
    // Every operation of f's own body that can block, each with the first
    // chain found below it: one report per blocking site, not only the
    // first, since a real function usually has several and a reader fixes
    // them one by one.
    llvm::SmallVector<Finding> blockingSitesIn(cir::FuncOp f);

private:
    ModuleOp module;
    llvm::DenseMap<Operation *, std::optional<Finding>> memo;
    llvm::DenseSet<Operation *> inProgress;
    // Set when a walk met a function still being walked further up: its
    // findings are not in yet, so a "nothing blocks" answer reached under
    // it is only provisional and must not be remembered.
    bool metCycle = false;
    // Stack slots holding the shared_ptr of a borrowed pop of an inline
    // queue: the destructor run on them frees nothing.
    llvm::DenseSet<Value> ownerlessSlots;

    void collectExemptions();
    bool ownerless(Value v) const;
    std::optional<Finding> blockingAt(Operation *op);
};

void ProgressChecker::collectExemptions() {
    module.walk([&](QueuePopOp pop) {
        if (pop.getBorrowed() && pop.getImpl() == Impl::spsc_inline && pop.getOut())
            ownerlessSlots.insert(pop.getOut());
    });
}

bool ProgressChecker::ownerless(Value v) const {
    while (auto up = v.getDefiningOp<cir::BaseClassAddrOp>()) v = up.getDerivedAddr();
    return ownerlessSlots.contains(v);
}

std::optional<Finding> ProgressChecker::blockingAt(Operation *op) {
    if (auto p = dyn_cast<ProgressOpInterface>(op)) {
        if (p.getProgress() == Progress::blocking || p.mayCallAllocator())
            return Finding{op, p.getBlockingReason(), {}};
        return std::nullopt;
    }
    if (auto asmOp = dyn_cast<cir::InlineAsmOp>(op)) {
        if (onlyNonBlockingInstructions(asmOp.getAsmString())) return std::nullopt;
        return Finding{op, "runs inline assembly, which is not looked into", {}};
    }
    if (op->getName().getStringRef().contains("asm"))
        return Finding{op, "runs inline assembly, which is not looked into", {}};
    // ClangIR spells a throw as operations, not as calls into the runtime.
    if (isa<cir::ThrowOp, cir::AllocExceptionOp>(op))
        return Finding{op, "throws: the C++ runtime allocates the exception and unwinds the stack", {}};
    auto call = dyn_cast<cir::CallOp>(op);
    if (!call) return std::nullopt;

    llvm::SmallVector<cir::FuncOp, 4> targets;
    if (cir::FuncOp callee = lookupCallee(call)) {
        // The shared_ptr of a borrowed pop owns nothing: its members free
        // nothing either.
        if (call.getNumOperands() > 0 && ownerless(call.getOperand(0))) return std::nullopt;
        targets.push_back(callee);
    } else {
        targets = indirectCallees(call);
        if (targets.empty())
            return Finding{op, "makes an indirect call whose target is not known here", {}};
    }
    for (cir::FuncOp t : targets) {
        if (t.getBody().empty()) {
            if (touchesOnlyItsArguments(t)) continue;
            return Finding{op, externalReason(t.getSymName()), {}};
        }
        if (auto f = blockingIn(t)) {
            Finding out = *f;
            out.chain.insert(out.chain.begin(), op);
            out.via.insert(out.via.begin(), t);
            return out;
        }
    }
    return std::nullopt;
}

std::optional<Finding> ProgressChecker::blockingIn(cir::FuncOp f) {
    if (auto it = memo.find(f); it != memo.end()) return it->second;
    if (!inProgress.insert(f).second) {
        metCycle = true;  // its own walk, further up, reports what it finds
        return std::nullopt;
    }
    const bool outer = metCycle;
    metCycle = false;
    std::optional<Finding> found;
    f.walk([&](Operation *op) {
        if ((found = blockingAt(op))) return WalkResult::interrupt();
        return WalkResult::advance();
    });
    inProgress.erase(f);
    if (found || !metCycle) memo[f] = found;
    metCycle = outer || metCycle;
    return found;
}

llvm::SmallVector<Finding> ProgressChecker::blockingSitesIn(cir::FuncOp f) {
    llvm::SmallVector<Finding> sites;
    // A site is reported once per distinct chain of source locations and
    // reason. Distinct calls inside one macro expansion share a location
    // (spdlog's catch clauses expand to several calls at one line) and are
    // merged; that changes how many notes repeat a finding, never whether
    // the function is reported.
    llvm::StringSet<> seen;
    auto key = [](const Finding &found) {
        std::string k;
        llvm::raw_string_ostream os(k);
        for (Operation *call : found.chain) os << call->getLoc() << ';';
        os << found.leaf->getLoc() << ';' << found.why;
        return k;
    };
    // f is marked as being walked so that a call back into it is a cycle,
    // whose sites this walk reports itself; f's own answer is not memoised.
    inProgress.insert(f);
    f.walk([&](Operation *op) {
        if (auto found = blockingAt(op))
            if (seen.insert(key(*found)).second) sites.push_back(*found);
    });
    inProgress.erase(f);
    metCycle = false;
    return sites;
}

class WTCCheckProgressPass : public PassWrapper<WTCCheckProgressPass, OperationPass<ModuleOp>> {
public:
    MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(WTCCheckProgressPass)

    StringRef getArgument() const final { return "wtc-check-progress"; }
    StringRef getDescription() const final {
        return "Fail the build when a function declared lock-free can reach a blocking operation "
               "(run after --wtc-specialize)";
    }
    void getDependentDialects(DialectRegistry &registry) const override {
        registry.insert<mlir::wtc::WTCDialect>();
        registry.insert<cir::CIRDialect>();
    }

    void runOnOperation() override {
        ModuleOp module = getOperation();
        ProgressChecker checker(module);
        bool failed = false;
        module.walk([&](cir::FuncOp f) {
            if (!hasAnnotation(f, "wtc_progress_lock_free") || f.getBody().empty()) return;
            const std::string name = llvm::demangle(f.getSymName().str());
            auto sites = checker.blockingSitesIn(f);
            if (sites.empty()) {
                f.emitRemark() << "wtc: " << name << " is declared lock-free: nothing it reaches blocks";
                return;
            }
            failed = true;
            InFlightDiagnostic d = f.emitError() << "wtc: " << name << " is declared lock-free but can block";
            // Enough chains to act on; the rest are counted, not listed.
            constexpr size_t kMaxSites = 8;
            for (Finding &found : llvm::MutableArrayRef(sites).take_front(kMaxSites)) {
                for (auto [call, target] : llvm::zip(found.chain, found.via)) {
                    const bool indirect = !lookupCallee(cast<cir::CallOp>(call));
                    d.attachNote(call->getLoc()) << "calls " << llvm::demangle(target.getSymName().str())
                                                 << (indirect ? " through a pointer" : "");
                }
                d.attachNote(found.leaf->getLoc()) << "which " << found.why;
            }
            if (const size_t more = sites.size() > kMaxSites ? sites.size() - kMaxSites : 0)
                d.attachNote(f.getLoc()) << "and " << more << " more blocking site"
                                         << (more == 1 ? "" : "s") << ", not shown";
        });
        if (failed) signalPassFailure();
        markAllAnalysesPreserved();
    }
};

} // namespace

namespace mlir::wtc {
void registerWTCCheckProgressPass() { PassRegistration<WTCCheckProgressPass>(); }
} // namespace mlir::wtc
