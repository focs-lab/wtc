#include "WTC/Transforms/Blocking.h"
#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"
#include "WTC/WTCInterfaces.h"
#include "WTC/WTCOps.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace mlir::wtc;

namespace mlir::wtc {

// Known offenders get their reason; everything else is opaque, since a
// function that cannot be seen into cannot be cleared. So is a body that
// may be replaced at link time.
std::optional<ExternalBlocking> externalBlocking(cir::FuncOp f) {
    if (touchesOnlyItsArguments(f)) return std::nullopt;
    const llvm::StringRef mangled = f.getSymName();
    const std::string name = llvm::demangle(mangled.str());
    const llvm::StringRef demangled(name);
    auto is = [&](BlockingKind kind, llvm::StringRef what, bool threading = false) {
        return ExternalBlocking{kind, (what + " (" + name + ")").str(), threading};
    };
    // DPDK's ring, stack and mempool constructors allocate from the EAL
    // heap, and take the EAL's tail-queue lock to register the object by
    // name first.
    static const llvm::StringSet<> registering = {
        "rte_ring_create",         "rte_ring_create_elem",           "rte_stack_create",
        "rte_mempool_create",      "rte_mempool_create_empty",       "rte_pktmbuf_pool_create",
        "rte_pktmbuf_pool_create_by_ops", "rte_pktmbuf_pool_create_extbuf"};
    if (registering.contains(mangled))
        return is(BlockingKind::Waits, "takes a DPDK library lock (the EAL tail-queue lock)");
    // The allocator: the C library's, DPDK's heap, and the replaceable
    // global operator new and delete by their exact mangled names -- plain,
    // array, aligned, sized and nothrow -- since a class's operator new or a
    // placement form taking the program's own arguments (a pool, say) is the
    // program's code, which may take any lock.
    static const llvm::StringSet<> freeing = {"realloc", "free", "rte_free"};
    static const llvm::Regex newDelete("^_Z(n[wa][mj](St11align_val_t)?(RKSt9nothrow_t)?"
                                       "|d[la]Pv[mj]?(St11align_val_t)?(RKSt9nothrow_t)?)$");
    if (isAllocator(f) || freeing.contains(mangled) || newDelete.match(mangled))
        return is(BlockingKind::Allocates, "calls the allocator");

    // The threading library: its locks, conditions, joins, one-time
    // initialisation and yields wait for another thread; the rest of it
    // (starting a thread, its attributes, its keys) is still a call into it.
    static const llvm::StringSet<> waits = {"pthread_join", "pthread_timedjoin_np", "pthread_tryjoin_np",
                                            "pthread_once", "thrd_join", "call_once", "syscall"};
    for (llvm::StringRef p : {"pthread_mutex_", "pthread_cond_", "pthread_rwlock_", "pthread_spin_",
                              "pthread_barrier_", "sem_", "mtx_", "cnd_"})
        if (mangled.starts_with(p)) return is(BlockingKind::Waits, "waits in the threading library", true);
    if (waits.contains(mangled) || mangled.contains("futex") || demangled.starts_with("std::call_once") ||
        demangled.starts_with("std::thread::join") || demangled.starts_with("std::condition_variable::wait") ||
        demangled.contains("condition_variable_any::wait"))
        return is(BlockingKind::Waits, "waits in the threading library", true);
    if (mangled == "sched_yield" || mangled == "thrd_yield" || mangled == "pthread_yield")
        return is(BlockingKind::Waits, "sleeps or yields", true);
    if (mangled.starts_with("pthread_") || mangled.starts_with("thrd_"))
        return is(BlockingKind::Threading, "calls into the threading library", true);
    // Waits outside the threading library: for another process or thread
    // through a System V semaphore or a file lock, for a descriptor to
    // become ready or a connection to arrive, for a child or a signal, for
    // a DPDK worker lcore to finish.
    static const llvm::StringSet<> otherWaits = {
        "semop",       "semtimedop", "flock",      "lockf",       "poll",        "ppoll",
        "epoll_wait",  "epoll_pwait", "select",    "pselect",     "recv",        "recvfrom",
        "recvmsg",     "recvmmsg",   "accept",     "accept4",     "wait",        "waitpid",
        "wait3",       "wait4",      "waitid",     "pause",       "sigwait",     "sigwaitinfo",
        "sigtimedwait", "sigsuspend", "rte_eal_wait_lcore", "rte_eal_mp_wait_lcore", "rte_thread_join"};
    if (otherWaits.contains(mangled)) return is(BlockingKind::Waits, "waits for another thread or process");
    // A function-local static's guard waits for the thread running its
    // initialiser.
    if (mangled.starts_with("__cxa_guard_acquire"))
        return is(BlockingKind::Waits, "takes a static-initialisation lock");

    if (mangled == "printf" || mangled == "fprintf" || mangled == "puts" || mangled == "fputs" ||
        mangled == "write" || mangled == "read" || mangled == "fwrite" || mangled == "fflush" ||
        demangled.starts_with("std::basic_ostream") || demangled.starts_with("std::operator<<"))
        return is(BlockingKind::InputOutput, "does input or output");
    if (mangled == "sleep" || mangled == "usleep" || mangled == "nanosleep")
        return is(BlockingKind::Sleeps, "sleeps or yields");
    if (mangled.starts_with("__cxa_throw") || mangled.starts_with("__cxa_allocate_exception") ||
        mangled.starts_with("__cxa_rethrow"))
        return is(BlockingKind::Throws, "throws");
    if (!f.getBody().empty())
        return ExternalBlocking{BlockingKind::Opaque, "calls " + name +
                                                          ", whose body here may be replaced at link time, so it "
                                                          "is counted as blocking"};
    return ExternalBlocking{BlockingKind::Opaque,
                            "calls " + name + ", whose body is not visible here, so it is counted as blocking"};
}

std::optional<std::string> allocatorCallbackRegistration(ModuleOp module) {
    static const llvm::StringSet<> names = {"rte_mem_event_callback_register", "rte_mem_alloc_validator_register",
                                            "__malloc_hook",   "__free_hook",    "__realloc_hook",
                                            "__memalign_hook", "__after_morecore_hook"};
    auto registers = [](llvm::StringRef sym) {
        return names.contains(sym) || llvm::StringRef(llvm::demangle(sym.str())).starts_with("std::set_new_handler");
    };
    std::optional<std::string> found;
    module.walk([&](Operation *op) {
        llvm::StringRef sym;
        if (auto call = dyn_cast<cir::CallOp>(op)) {
            if (auto callee = call.getCalleeAttr()) sym = callee.getValue();
        } else if (auto gg = dyn_cast<cir::GetGlobalOp>(op)) {
            sym = gg.getName();
        }
        if (sym.empty() || !registers(sym)) return WalkResult::advance();
        found = llvm::demangle(sym.str());
        return WalkResult::interrupt();
    });
    return found;
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
//
// Inert is narrower: prefetches, no-ops and the time-stamp counter, which
// order nothing. A fence, the spin-wait hint and the compiler barrier are
// what a hand-written wait is made of, and are refused.
bool onlyNonBlockingInstructions(llvm::StringRef text, bool inert) {
    static const llvm::StringSet<> bare = {"pause",  "lfence", "mfence", "sfence", "rdtsc",
                                           "rdtscp", "nop",    "yield",  "isb"};
    static const llvm::StringSet<> bareInert = {"rdtsc", "rdtscp", "nop"};
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
        if (inert && !bareInert.contains(mnemonic)) return false;
        return rest.empty() || rest.starts_with("#");
    };
    bool any = false;
    llvm::SmallString<64> joined;
    for (size_t i = 0; i < stmts.size(); ++i) {
        llvm::StringRef stmt = lowered(stmts[i].trim());
        stmt = stmt.split("//").first.rtrim();
        if (stmt.empty() || stmt.starts_with("#")) continue;  // nothing, or a comment
        any = true;
        if ((stmt == "rep" || stmt == "lock") && i + 1 < stmts.size()) {
            joined = stmt;
            joined += ' ';
            joined += lowered(stmts[++i].trim());
            stmt = llvm::StringRef(joined).split("//").first.rtrim();
        }
        if (bareAlone(stmt)) continue;
        // "rep; nop" and "rep nop" are pause, spelled out.
        if (stmt.starts_with("rep ") || stmt.starts_with("rep\t")) {
            if (inert || !bareAlone(stmt.drop_front(4).ltrim(), "nop")) return false;
            continue;
        }
        if (x86Prefetch.match(stmt) || armPrefetch.match(stmt)) continue;
        if (!inert && (armBarrier.match(stmt) || stackFence.match(stmt))) continue;
        return false;
    }
    return any || !inert;
}

BlockingQuery::BlockingQuery(ModuleOp module, BlockingKinds accepted, Extra extra)
    : accepted(accepted), extra(std::move(extra)) {
    module.walk([&](QueuePopOp pop) {
        if (pop.getBorrowed() && pop.getImpl() == Impl::spsc_inline && pop.getOut())
            ownerlessSlots.insert(pop.getOut());
    });
}

bool BlockingQuery::ownerless(Value v) const {
    while (auto up = v.getDefiningOp<cir::BaseClassAddrOp>()) v = up.getDerivedAddr();
    return ownerlessSlots.contains(v);
}

std::optional<BlockingSite> BlockingQuery::site(Operation *leaf, BlockingKind kind, std::string why) const {
    if (accepted.contains(kind)) return std::nullopt;
    return BlockingSite{leaf, kind, std::move(why), cir::FuncOp(), false, {}, {}};
}

std::optional<BlockingSite> BlockingQuery::siteAt(Operation *op, bool inCallee) {
    if (extra)
        if (auto s = extra(op, inCallee)) return s;
    if (auto p = dyn_cast<ProgressOpInterface>(op)) {
        if (p.getProgress() == Progress::blocking) return site(op, BlockingKind::Waits, p.getBlockingReason());
        if (p.mayCallAllocator()) return site(op, BlockingKind::Allocates, p.getBlockingReason());
        return std::nullopt;
    }
    if (auto asmOp = dyn_cast<cir::InlineAsmOp>(op)) {
        if (onlyNonBlockingInstructions(asmOp.getAsmString())) return std::nullopt;
        return site(op, BlockingKind::Assembly, "runs inline assembly, which is not looked into");
    }
    if (op->getName().getStringRef().contains("asm"))
        return site(op, BlockingKind::Assembly, "runs inline assembly, which is not looked into");
    // ClangIR spells a throw as operations, not as calls into the runtime.
    if (isa<cir::ThrowOp, cir::AllocExceptionOp>(op))
        return site(op, BlockingKind::Throws, "throws: the C++ runtime allocates the exception and unwinds the stack");
    if (auto call = dyn_cast<cir::CallOp>(op)) return callSite(call);
    return std::nullopt;
}

std::optional<BlockingSite> BlockingQuery::callSite(cir::CallOp call) {
    llvm::SmallVector<cir::FuncOp, 4> targets;
    if (cir::FuncOp callee = lookupCallee(call)) {
        // The shared_ptr of a borrowed pop owns nothing: its members free
        // nothing either.
        if (call.getNumOperands() > 0 && ownerless(call.getOperand(0))) return std::nullopt;
        targets.push_back(callee);
    } else {
        targets = indirectCallees(call);
        if (targets.empty())
            return site(call, BlockingKind::UnknownTarget, "makes an indirect call whose target is not known here");
    }
    for (cir::FuncOp t : targets) {
        // A weak or non-ODR linkonce body, or a common or extern_weak
        // symbol, may be replaced at link time by another translation
        // unit's, which may do anything: such a body is not followed.
        if (t.getBody().empty() || cir::isInterposableLinkage(t.getLinkage())) {
            auto ext = externalBlocking(t);
            auto make = [&](BlockingKind kind, std::string why) {
                return BlockingSite{call, kind, std::move(why), t, ext && ext->threading, {}, {}};
            };
            // A wait is the reason that matters most, then a throw, which
            // leaves without passing what follows, then the rest.
            const bool waits = ext && (ext->kind == BlockingKind::Waits || ext->kind == BlockingKind::Threading);
            if (waits && !accepted.contains(ext->kind)) return make(ext->kind, ext->what);
            if (!call.getNothrow() && !accepted.contains(BlockingKind::MayThrow))
                return make(BlockingKind::MayThrow, "may throw (" + llvm::demangle(t.getSymName().str()) + ")");
            if (ext && !accepted.contains(ext->kind)) return make(ext->kind, ext->what);
            continue;
        }
        if (auto f = firstIn(t)) {
            BlockingSite out = *f;
            out.chain.insert(out.chain.begin(), call);
            out.via.insert(out.via.begin(), t);
            return out;
        }
    }
    return std::nullopt;
}

std::optional<BlockingSite> BlockingQuery::firstIn(cir::FuncOp f) {
    if (auto it = memo.find(f); it != memo.end()) return it->second;
    if (!inProgress.insert(f).second) {
        metCycle = true;  // its own walk, further up, reports what it finds
        return std::nullopt;
    }
    const bool outer = metCycle;
    metCycle = false;
    std::optional<BlockingSite> found;
    f.walk([&](Operation *op) {
        if ((found = siteAt(op, /*inCallee=*/true))) return WalkResult::interrupt();
        return WalkResult::advance();
    });
    inProgress.erase(f);
    if (found || !metCycle) memo[f] = found;
    metCycle = outer || metCycle;
    return found;
}

llvm::SmallVector<BlockingSite> BlockingQuery::sitesIn(cir::FuncOp f) {
    llvm::SmallVector<BlockingSite> sites;
    // A site is reported once per distinct chain of source locations and
    // reason. Distinct calls inside one macro expansion share a location
    // (spdlog's catch clauses expand to several calls at one line) and are
    // merged; that changes how many notes repeat a finding, never whether
    // the function is reported.
    llvm::StringSet<> seen;
    auto key = [](const BlockingSite &found) {
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
        if (auto found = siteAt(op, /*inCallee=*/false))
            if (seen.insert(key(*found)).second) sites.push_back(*found);
    });
    inProgress.erase(f);
    metCycle = false;
    return sites;
}

} // namespace mlir::wtc
