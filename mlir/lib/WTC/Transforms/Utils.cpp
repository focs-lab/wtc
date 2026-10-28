#include "WTC/Transforms/Utils.h"
#include "WTC/WTCAttrs.h"

#include "clang/CIR/Dialect/IR/CIRAttrs.h"
#include "mlir/IR/BuiltinOps.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Demangle/Demangle.h"

#include <cstdlib>

namespace mlir::wtc {

cir::FuncOp lookupCallee(cir::CallOp op) {
    auto calleeAttr = op.getCalleeAttr();
    if (!calleeAttr)
        return nullptr;
    auto module = op->getParentOfType<mlir::ModuleOp>();
    if (!module)
        return nullptr;
    Operation *callee = mlir::SymbolTable::lookupNearestSymbolFrom(module, calleeAttr);
    return mlir::dyn_cast_or_null<cir::FuncOp>(callee);
}

bool hasAnnotation(cir::FuncOp funcOp, StringRef annotation) {
    if (!funcOp)
        return false;
    auto annotations = funcOp.getAnnotationsAttr();
    if (!annotations)
        return false;
    for (auto attr : annotations) {
        auto annotAttr = mlir::dyn_cast<cir::AnnotationAttr>(attr);
        if (!annotAttr)
            continue;
        if (annotAttr.getName().getValue() == annotation)
            return true;
    }
    return false;
}

bool isWTCAnnotatedCall(cir::CallOp op, StringRef annotation) {
    return hasAnnotation(lookupCallee(op), annotation);
}

// Standard entry points that mean what the shim's annotations mean. Only
// names with a settled meaning belong here: pthread_create starts a thread
// in every C program that has one, and a program that defines its own
// function with that name is already in trouble.
//
// libstdc++'s std::thread constructor is a template instantiated in the
// translation unit, and it hands the callable to a bodiless
// std::thread::_M_start_thread. Its entry cannot be resolved -- it travels
// inside a heap-allocated state object -- but recognising the call matters
// anyway: it is where the start-up phase ends, and without it every
// program that uses std::thread looked as if it never started a thread.
static SemanticOpKind kindByName(StringRef name) {
    if (name == "pthread_create" || name == "thrd_create") return SemanticOpKind::ThreadSpawn;
    // DPDK's EAL starts a worker on another lcore: rte_eal_remote_launch(f, arg,
    // worker_id), entry first and its argument next, as the shim's spawn.
    if (name == "rte_eal_remote_launch") return SemanticOpKind::ThreadSpawn;
    // rte_eal_mp_remote_launch(f, arg, call_main): f on every worker lcore,
    // which ThreadInstanceWalk::siteMult counts as many.
    if (name == "rte_eal_mp_remote_launch") return SemanticOpKind::ThreadSpawn;
    if (name.contains("6thread15_M_start_thread")) return SemanticOpKind::ThreadSpawn;
    return SemanticOpKind::Unknown;
}

SemanticOpKind getSemanticKind(cir::FuncOp funcOp) {
    if (!funcOp)
        return SemanticOpKind::Unknown;
    if (SemanticOpKind k = kindByName(funcOp.getSymName()); k != SemanticOpKind::Unknown)
        return k;
    auto annotations = funcOp.getAnnotationsAttr();
    if (!annotations)
        return SemanticOpKind::Unknown;
    for (auto attr : annotations) {
        auto annotAttr = mlir::dyn_cast<cir::AnnotationAttr>(attr);
        // Other attributes may sit in the same array; skip them rather than
        // give up, which is what an earlier version did and which hid every
        // spawn behind one unrelated annotation.
        if (!annotAttr)
            continue;
        StringRef name = annotAttr.getName().getValue();
        if (name == "wtc_thread_spawn")       return SemanticOpKind::ThreadSpawn;
        if (name == "wtc_queue_construction") return SemanticOpKind::QueueConstruction;
        if (name == "wtc_queue_push")         return SemanticOpKind::QueuePush;
        if (name == "wtc_queue_try_pop")      return SemanticOpKind::QueueTryPop;
        if (name == "wtc_queue_try_pop_into") return SemanticOpKind::QueueTryPopInto;
        if (name == "wtc_queue_batch_begin")  return SemanticOpKind::QueueBatchBegin;
        if (name == "wtc_queue_push_deferred") return SemanticOpKind::QueuePushDeferred;
        if (name == "wtc_queue_batch_end")    return SemanticOpKind::QueueBatchEnd;
        if (name == "wtc_mutex_lock")         return SemanticOpKind::MutexLock;
        if (name == "wtc_mutex_unlock")       return SemanticOpKind::MutexUnlock;
        if (name == "wtc_mutex_batch_begin")  return SemanticOpKind::MutexBatchBegin;
        if (name == "wtc_mutex_batch_step")   return SemanticOpKind::MutexBatchStep;
        if (name == "wtc_mutex_batch_end")    return SemanticOpKind::MutexBatchEnd;
    }
    return SemanticOpKind::Unknown;
}

namespace {
// A single-producer single-consumer class: how its name appears inside a
// mangled nested name (a cheap filter), the class as demangled, and the
// methods that push and those that pop. Anything else -- size, empty,
// capacity, the destructor -- is neither.
struct LibraryClass {
    StringRef mangled, qualified;
    llvm::ArrayRef<StringLiteral> pushes, pops;
};
constexpr StringLiteral kRigtorpPush[] = {"emplace", "try_emplace", "push", "try_push"};
constexpr StringLiteral kRigtorpPop[] = {"front", "pop"};
constexpr StringLiteral kFollyPush[] = {"write"};
constexpr StringLiteral kFollyPop[] = {"read", "frontPtr", "popFront"};
constexpr StringLiteral kBoostPush[] = {"push"};
constexpr StringLiteral kBoostPop[] = {"pop", "consume_one", "consume_all", "front"};
constexpr StringLiteral kMoodyPush[] = {"enqueue", "try_enqueue", "emplace", "try_emplace"};
constexpr StringLiteral kMoodyPop[] = {"try_dequeue", "peek", "pop", "wait_dequeue", "wait_dequeue_timed"};
const LibraryClass kLibraryClasses[] = {
    {"7rigtorp9SPSCQueue", "rigtorp::SPSCQueue", kRigtorpPush, kRigtorpPop},
    {"5folly21ProducerConsumerQueue", "folly::ProducerConsumerQueue", kFollyPush, kFollyPop},
    {"5boost8lockfree10spsc_queue", "boost::lockfree::spsc_queue", kBoostPush, kBoostPop},
    {"10moodycamel17ReaderWriterQueue", "moodycamel::ReaderWriterQueue", kMoodyPush, kMoodyPop},
    {"10moodycamel25BlockingReaderWriterQueue", "moodycamel::BlockingReaderWriterQueue", kMoodyPush,
     kMoodyPop},
};

// The class among those above whose member `mangled` is, with the member's
// base name and whether it is a constructor; null for anything else,
// including a function that only takes such a queue as an argument. A name
// may mention several of the classes (a queue of queues); the member's own
// class is the one its context starts with.
const LibraryClass *libraryClassOf(StringRef mangled, std::string &base, bool &ctor) {
    if (!mangled.starts_with("_Z")) return nullptr;
    if (llvm::none_of(kLibraryClasses, [&](const LibraryClass &c) { return mangled.contains(c.mangled); }))
        return nullptr;
    // The demangler's nodes point into the name it parsed, so the copy
    // lives as long as they are read.
    const std::string name = mangled.str();
    llvm::ItaniumPartialDemangler d;
    if (d.partialDemangle(name.c_str()) || !d.isFunction()) return nullptr;
    size_t n = 0;
    char *c = d.getFunctionDeclContextName(nullptr, &n);
    const std::string context = c ? c : "";
    std::free(c);
    const LibraryClass *cls = nullptr;
    for (const LibraryClass &k : kLibraryClasses)
        if (mangled.contains(k.mangled) && StringRef(context).starts_with((k.qualified + "<").str()))
            cls = &k;
    if (!cls) return nullptr;
    char *b = d.getFunctionBaseName(nullptr, &n);
    base = b ? b : "";
    std::free(b);
    ctor = d.isCtorOrDtor() && !StringRef(base).starts_with("~");
    return cls;
}

bool isDpdkRingFunction(StringRef n) { return n.starts_with("rte_ring_") || n.starts_with("__rte_ring"); }

// DPDK's mempool library and the mbuf library built on it; before DPDK
// 21.11 the mempool's inline put and get were __mempool_generic_put and
// __mempool_generic_get.
bool isDpdkMempoolFunction(StringRef n) {
    return n.starts_with("rte_mempool_") || n.starts_with("__rte_mempool") || n.starts_with("__mempool_") ||
           n.starts_with("rte_pktmbuf_") || n.starts_with("__rte_pktmbuf") || n.starts_with("rte_mbuf_") ||
           n.starts_with("__rte_mbuf");
}

// The functions of DPDK's mempool and mbuf libraries (26.11, and older
// names) that neither put nor get: they read or set metadata, build or
// look up a pool, iterate, link mbufs (which a free then has to follow:
// isMbufLinkFunction), or release a pool. Any other unmodelled function of
// those libraries that is handed a pool or an mbuf is taken to put or get.
bool isInertMempoolFunction(StringRef n) {
    static const llvm::StringSet<> inert = {
        // rte_mempool.h
        "rte_mempool_avail_count", "rte_mempool_in_use_count", "rte_mempool_full", "rte_mempool_empty",
        "rte_mempool_count", "rte_mempool_free_count", "rte_mempool_get_priv", "rte_mempool_get_ops",
        "rte_mempool_get_header", "rte_mempool_get_trailer", "rte_mempool_from_obj", "rte_mempool_virt2iova",
        "rte_mempool_get_page_size", "rte_mempool_get_mem_range", "rte_mempool_get_obj_alignment",
        "rte_mempool_calc_obj_size", "rte_mempool_dump", "rte_mempool_list_dump", "rte_mempool_audit",
        "rte_mempool_walk", "rte_mempool_lookup", "rte_mempool_free", "rte_mempool_obj_iter",
        "rte_mempool_mem_iter", "rte_mempool_cache_create", "rte_mempool_cache_free",
        "rte_mempool_default_cache", "rte_mempool_check_cookies", "rte_mempool_contig_blocks_check_cookies",
        "rte_mempool_stats_reset", "rte_mempool_register_ops", "rte_mempool_ops_get_info",
        "rte_mempool_ops_get_count", "rte_mempool_ops_alloc", "rte_mempool_ops_free",
        "rte_mempool_ops_calc_mem_size", "rte_mempool_op_calc_mem_size_default",
        "rte_mempool_op_calc_mem_size_helper", "rte_mempool_event_callback_register",
        "rte_mempool_event_callback_unregister", "rte_mempool_trace_ops_alloc", "__mempool_check_cookies",
        "__mempool_contig_blocks_check_cookies",
        // rte_mbuf.h, rte_mbuf_core.h, rte_mbuf_dyn.h, rte_mbuf_pool_ops.h
        "rte_mbuf_refcnt_read", "rte_mbuf_refcnt_set", "rte_mbuf_refcnt_update", "__rte_mbuf_refcnt_update",
        "rte_mbuf_ext_refcnt_read", "rte_mbuf_ext_refcnt_set", "rte_mbuf_ext_refcnt_update",
        "rte_mbuf_buf_addr", "rte_mbuf_data_addr_default", "rte_mbuf_data_iova", "rte_mbuf_data_iova_default",
        "rte_mbuf_iova_get", "rte_mbuf_iova_set", "rte_mbuf_to_baddr", "rte_mbuf_to_priv",
        "rte_mbuf_from_indirect", "rte_mbuf_prefetch_part1", "rte_mbuf_prefetch_part2",
        "rte_mbuf_sanity_check", "rte_mbuf_raw_sanity_check", "__rte_mbuf_sanity_check",
        "__rte_mbuf_raw_sanity_check", "rte_mbuf_check", "rte_mbuf_raw_check", "rte_mbuf_raw_reset_bulk",
        "rte_mbuf_sched_get", "rte_mbuf_sched_set", "rte_mbuf_sched_queue_get", "rte_mbuf_sched_queue_set",
        "rte_mbuf_sched_color_get", "rte_mbuf_sched_color_set", "rte_mbuf_sched_traffic_class_get",
        "rte_mbuf_sched_traffic_class_set", "rte_mbuf_tx_offload", "rte_mbuf_dynfield_copy",
        "rte_mbuf_dynfield_lookup", "rte_mbuf_dynfield_register", "rte_mbuf_dynfield_register_offset",
        "rte_mbuf_dynflag_lookup", "rte_mbuf_dynflag_register", "rte_mbuf_dynflag_register_bitnum",
        "rte_mbuf_dyn_dump", "rte_mbuf_dyn_rx_timestamp_register", "rte_mbuf_dyn_tx_timestamp_register",
        "rte_mbuf_history_dump", "rte_mbuf_history_dump_all", "rte_mbuf_history_dump_mempool",
        "rte_mbuf_history_init", "rte_mbuf_history_mark", "rte_mbuf_history_mark_bulk",
        "rte_mbuf_best_mempool_ops", "rte_mbuf_platform_mempool_ops", "rte_mbuf_user_mempool_ops",
        "rte_mbuf_set_platform_mempool_ops", "rte_mbuf_set_user_mempool_ops", "rte_pktmbuf_adj",
        "rte_pktmbuf_append", "rte_pktmbuf_prepend", "rte_pktmbuf_trim", "rte_pktmbuf_data_len",
        "rte_pktmbuf_pkt_len", "rte_pktmbuf_headroom", "rte_pktmbuf_tailroom", "rte_pktmbuf_lastseg",
        "rte_pktmbuf_reset", "rte_pktmbuf_reset_headroom", "rte_pktmbuf_read", "__rte_pktmbuf_read",
        "rte_pktmbuf_dump", "rte_pktmbuf_is_contiguous", "rte_pktmbuf_iova", "rte_pktmbuf_iova_offset",
        "rte_pktmbuf_priv_size", "rte_pktmbuf_priv_flags", "rte_pktmbuf_data_room_size",
        "rte_pktmbuf_ext_shinfo_init_helper", "rte_pktmbuf_refcnt_update", "rte_pktmbuf_pool_init",
        "rte_pktmbuf_init", "__rte_pktmbuf_copy_hdr", "rte_pktmbuf_chain", "rte_pktmbuf_attach",
        "rte_pktmbuf_attach_extbuf"};
    return inert.contains(n);
}

// A mempool function as the model reads it (rte_mempool.h, rte_mbuf.h,
// DPDK 26.11). A pool's ops name its sides: "ring_sp_sc", "ring_sp_mc",
// "ring_mp_sc" and "ring_mp_mc", picked from RTE_MEMPOOL_F_SP_PUT and
// RTE_MEMPOOL_F_SC_GET by rte_mempool_create and, since DPDK 23.11, by
// rte_mempool_create_empty, or set by name. Every put and get goes through
// the ops, so every one follows them. A per-lcore cache changes when a put
// or a get reaches the pool's ring, not which threads may: each lcore
// flushes and refills its own cache, and a thread with no lcore has none,
// so every put and get is counted on the thread that makes it.
LibraryQueueFunction classifyMempool(StringRef n) {
    LibraryQueueFunction r;
    r.member = true;
    r.kind = LibraryQueueKind::Mempool;
    r.sync = LibraryQueueSync::ByFlags;
    auto create = [&](std::optional<unsigned> flags, std::optional<unsigned> ops) {
        r.role = LibraryQueueRole::Create;
        r.flagsArg = flags;
        r.opsArg = ops;
    };
    auto op = [&](LibraryQueueRole role, unsigned arg = 0) {
        r.role = role;
        r.queueArg = arg;
    };
    // rte_mempool_create(name, n, elt_size, cache_size, private_data_size,
    //   mp_init, mp_init_arg, obj_init, obj_init_arg, socket_id, flags),
    // rte_mempool_create_empty(name, n, elt_size, cache_size,
    //   private_data_size, socket_id, flags),
    // rte_pktmbuf_pool_create_by_ops(name, n, cache_size, priv_size,
    //   data_room_size, socket_id, ops_name). rte_pktmbuf_pool_create and
    // _extbuf take the platform's or the command line's default ops, which
    // the program does not choose: nothing is declared single.
    if (n == "rte_mempool_create") create(10, std::nullopt);
    else if (n == "rte_mempool_create_empty") create(6, std::nullopt);
    else if (n == "rte_pktmbuf_pool_create_by_ops") create(std::nullopt, 6);
    else if (n == "rte_pktmbuf_pool_create" || n == "rte_pktmbuf_pool_create_extbuf") create(std::nullopt, std::nullopt);
    // rte_mempool_set_ops_byname(mp, name, pool_config) replaces the ops.
    else if (n == "rte_mempool_set_ops_byname") {
        op(LibraryQueueRole::Construct);
        r.opsArg = 1;
    }
    // Populating a pool puts its objects in, as its creation does when it
    // populates: part of building it, before any thread takes from it.
    else if (n == "rte_mempool_populate_default" || n == "rte_mempool_populate_anon" ||
             n == "rte_mempool_populate_iova" || n == "rte_mempool_populate_virt" ||
             n == "rte_mempool_ops_populate" || n == "rte_mempool_op_populate_default" ||
             n == "rte_mempool_op_populate_helper")
        op(LibraryQueueRole::Construct);
    else if (n == "rte_mempool_put" || n == "rte_mempool_put_bulk" || n == "rte_mempool_generic_put" ||
             n == "rte_mempool_do_generic_put" || n == "__mempool_generic_put" ||
             n == "rte_mempool_ops_enqueue_bulk" || n == "rte_mbuf_raw_free_bulk" ||
             n == "rte_mempool_mp_put" || n == "rte_mempool_mp_put_bulk")
        op(LibraryQueueRole::Push);
    // (cache, mp, ...): the flush, and the zero-copy cache access proposed
    // for DPDK (not in it as of 26.11), which reaches the ring as a flush
    // or a refill does.
    else if (n == "rte_mempool_cache_flush" || n == "rte_mempool_cache_zc_put_bulk")
        op(LibraryQueueRole::Push, 1);
    else if (n == "rte_mempool_cache_zc_get_bulk") op(LibraryQueueRole::Pop, 1);
    else if (n == "rte_mempool_get" || n == "rte_mempool_get_bulk" || n == "rte_mempool_generic_get" ||
             n == "rte_mempool_do_generic_get" || n == "__mempool_generic_get" ||
             n == "rte_mempool_get_contig_blocks" || n == "rte_mempool_ops_dequeue_bulk" ||
             n == "rte_mempool_ops_dequeue_contig_blocks" || n == "rte_pktmbuf_alloc_bulk" ||
             n == "rte_mbuf_raw_alloc_bulk" || n == "rte_mempool_mc_get" || n == "rte_mempool_mc_get_bulk")
        op(LibraryQueueRole::Pop);
    // The single-thread forms DPDK deprecated in 17.05 and removed after:
    // the call promises one thread on its side, as rte_ring_sp_* does. Their
    // multi-thread forms went through the ops, and follow them above.
    else if (n == "rte_mempool_sp_put" || n == "rte_mempool_sp_put_bulk") {
        op(LibraryQueueRole::Push);
        r.sync = LibraryQueueSync::Single;
    } else if (n == "rte_mempool_sc_get" || n == "rte_mempool_sc_get_bulk") {
        op(LibraryQueueRole::Pop);
        r.sync = LibraryQueueSync::Single;
    }
    else if (n == "rte_pktmbuf_alloc" || n == "rte_mbuf_raw_alloc") {
        op(LibraryQueueRole::Pop);
        r.returnsMbuf = true;
    } else if (n == "rte_pktmbuf_copy" || n == "rte_pktmbuf_clone") {   // (m, mp, ...)
        op(LibraryQueueRole::Pop, 1);
        r.returnsMbuf = true;
    }
    // A free puts into the pool the mbuf's `pool` field names.
    else if (n == "rte_mbuf_raw_free") {
        op(LibraryQueueRole::Push);
        r.viaMbuf = MbufFree::Own;
    } else if (n == "rte_pktmbuf_free" || n == "rte_pktmbuf_free_seg" || n == "rte_pktmbuf_free_bulk" ||
               n == "rte_pktmbuf_linearize" || n == "__rte_pktmbuf_linearize") {
        // rte_pktmbuf_linearize frees the segments it copies out of.
        op(LibraryQueueRole::Push);
        r.viaMbuf = MbufFree::Chain;
    } else if (n == "rte_pktmbuf_detach" || n == "rte_pktmbuf_prefree_seg" || n == "__rte_pktmbuf_free_direct") {
        // rte_pktmbuf_prefree_seg detaches an indirect mbuf, which may free
        // the buffer it was attached to.
        op(LibraryQueueRole::Push);
        r.viaMbuf = MbufFree::Attached;
    } else {
        r.unmodelled = !isInertMempoolFunction(n);
    }
    return r;
}

// What a function is, read from its name alone.
LibraryQueueFunction classify(StringRef n) {
    LibraryQueueFunction r;
    if (isDpdkMempoolFunction(n)) return classifyMempool(n);
    if (isDpdkRingFunction(n)) {
        r.member = true;
        r.kind = LibraryQueueKind::Ring;
        r.sync = LibraryQueueSync::ByFlags;
        // rte_ring_create(name, count, socket, flags),
        // rte_ring_create_elem(name, esize, count, socket, flags),
        // rte_ring_init(r, name, count, flags).
        if (n == "rte_ring_create") {
            r.role = LibraryQueueRole::Create;
            r.flagsArg = 3;
        } else if (n == "rte_ring_create_elem") {
            r.role = LibraryQueueRole::Create;
            r.flagsArg = 4;
        } else if (n == "rte_ring_init") {
            r.role = LibraryQueueRole::Construct;
            r.flagsArg = 3;
        } else if (n.contains("enqueue") || n.contains("dequeue")) {
            // Every enqueue and dequeue, bulk, burst, element-sized, peek
            // and zero-copy, takes the ring first. sp_ and sc_ name the
            // single-thread forms, mp_ and mc_ the multi-thread ones
            // (with their rts_ and hts_ kinds); the rest follow the flags.
            const bool push = n.contains("enqueue");
            r.role = push ? LibraryQueueRole::Push : LibraryQueueRole::Pop;
            if (n.contains(push ? "_sp_" : "_sc_")) r.sync = LibraryQueueSync::Single;
            else if (n.contains(push ? "_mp_" : "_mc_")) r.sync = LibraryQueueSync::Multi;
        }
        return r;
    }
    std::string base;
    bool ctor = false;
    const LibraryClass *cls = libraryClassOf(n, base, ctor);
    if (!cls) return r;
    r.member = true;
    r.sync = LibraryQueueSync::ByType;
    if (ctor) r.role = LibraryQueueRole::Construct;
    else if (llvm::is_contained(cls->pushes, base)) r.role = LibraryQueueRole::Push;
    else if (llvm::is_contained(cls->pops, base)) r.role = LibraryQueueRole::Pop;
    return r;
}
} // namespace

LibraryQueueFunction libraryQueueFunction(cir::FuncOp f) {
    return f ? classify(f.getSymName()) : LibraryQueueFunction();
}

LibraryQueueCall libraryQueueCall(cir::CallOp call, const LibraryQueueFunction &f) {
    if (!f) return {};
    LibraryQueueCall r;
    const unsigned k = firstRealOperand(call);
    r.role = f.role;
    r.sync = f.sync;
    r.kind = f.kind;
    r.queueOperand = k + f.queueArg;
    if (f.flagsArg) r.flagsOperand = k + *f.flagsArg;
    if (f.opsArg) r.opsOperand = k + *f.opsArg;
    r.viaMbuf = f.viaMbuf;
    r.returnsMbuf = f.returnsMbuf;
    if (r.role != LibraryQueueRole::Create && r.queueOperand >= call.getNumOperands()) return {};
    return r;
}

LibraryQueueCall libraryQueueCall(cir::CallOp call) {
    return libraryQueueCall(call, libraryQueueFunction(lookupCallee(call)));
}

bool isLibraryQueueFunction(cir::FuncOp f) { return libraryQueueFunction(f).member; }

bool isMbufLinkFunction(StringRef n) {
    return n == "rte_pktmbuf_chain" || n == "rte_pktmbuf_attach" || n == "rte_pktmbuf_attach_extbuf" ||
           n == "rte_pktmbuf_clone";
}

static bool dictHasSret(Attribute dictLike) {
    auto dict = mlir::dyn_cast_or_null<DictionaryAttr>(dictLike);
    return dict && dict.get("llvm.sret");
}

bool hasSretArg(cir::CallOp op, unsigned idx) {
    if (auto argAttrs = op.getArgAttrsAttr()) {
        if (idx < argAttrs.size() && dictHasSret(argAttrs[idx]))
            return true;
    }
    if (auto callee = lookupCallee(op)) {
        if (auto argAttrs = callee.getArgAttrsAttr()) {
            if (idx < argAttrs.size() && dictHasSret(argAttrs[idx]))
                return true;
        }
    }
    return false;
}

unsigned firstRealOperand(cir::CallOp op) {
    return (op.getNumOperands() > 0 && hasSretArg(op, 0)) ? 1u : 0u;
}

llvm::SmallVector<cir::FuncOp, 4> indirectCallees(cir::CallOp call) {
    llvm::SmallVector<cir::FuncOp, 4> out;
    if (!call.isIndirect()) return out;
    Value fnv = traceToRoot(call.getIndirectCall());
    if (auto gg = fnv.getDefiningOp<cir::GetGlobalOp>()) {
        if (auto f = mlir::SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(gg, gg.getNameAttr())) out.push_back(f);
        return out;
    }
    // A parameter of the enclosing function: the actuals at every call of
    // it, each a function's address, or nothing is known. Only a function
    // no other translation unit can call has all its calls here: one with
    // internal linkage (static, or in an anonymous namespace). An external
    // function, an inline one or a template instance (linkonce_odr) may be
    // called from elsewhere with any other pointer.
    auto arg = mlir::dyn_cast<BlockArgument>(fnv);
    if (!arg) return out;
    auto owner = mlir::dyn_cast_or_null<cir::FuncOp>(arg.getOwner()->getParentOp());
    if (!owner || owner.getBody().empty() || arg.getOwner() != &owner.getBody().front()) return out;
    if (!cir::isLocalLinkage(owner.getLinkage())) return out;
    const unsigned idx = arg.getArgNumber();
    auto module = owner->getParentOfType<ModuleOp>();
    auto uses = mlir::SymbolTable::getSymbolUses(owner, module);
    if (!uses) return out;
    for (const mlir::SymbolTable::SymbolUse &u : *uses) {
        auto site = mlir::dyn_cast<cir::CallOp>(u.getUser());
        if (!site || site.isIndirect()) { out.clear(); return out; }   // its address is taken
        const unsigned k = firstRealOperand(site) + idx;
        if (k >= site.getNumOperands()) { out.clear(); return out; }
        auto g = traceToRoot(site.getOperand(k)).getDefiningOp<cir::GetGlobalOp>();
        auto f = g ? mlir::SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(g, g.getNameAttr()) : cir::FuncOp();
        if (!f) { out.clear(); return out; }
        if (!llvm::is_contained(out, f)) out.push_back(f);
    }
    return out;
}

Value traceToLocalRoot(Value v) {
    while (true) {
        Operation* def = v.getDefiningOp();
        if (!def)
            return v;
        // Only the casts under which the value still names the same object:
        // a reinterpretation of the pointer, or an array decaying to a
        // pointer to its first element. A pointer made from an integer
        // names nothing the walk can vouch for, whatever the integer was
        // made from, and following it once placed `(S *)(uintptr_t)p + k`
        // on p's object.
        if (auto castOp = mlir::dyn_cast<cir::CastOp>(def)) {
            const cir::CastKind kind = castOp.getKind();
            if (kind != cir::CastKind::bitcast && kind != cir::CastKind::array_to_ptrdecay)
                return v;
            v = castOp.getSrc();
            continue;
        }
        if (auto uccOp = mlir::dyn_cast<mlir::UnrealizedConversionCastOp>(def)) {
            if (uccOp.getNumOperands() != 1)
                return v;
            v = uccOp.getOperand(0);
            continue;
        }
        return v;
    }
}

cir::FuncOp resolveFunctionOperand(Value v) {
    Value root = traceToLocalRoot(v);
    auto getGlobal = root.getDefiningOp<cir::GetGlobalOp>();
    if (!getGlobal)
        return nullptr;
    return mlir::SymbolTable::lookupNearestSymbolFrom<cir::FuncOp>(
        getGlobal, getGlobal.getNameAttr());
}

// The stored value if `alloca` is written exactly once, null stores aside,
// by a store in its own block that precedes every load, and nothing else
// touches its address.
// Whether a store into a slot runs before a load from it on every path:
// the store sits in the slot's own block, or in a nest of `cir.scope` and
// `cir.cleanup.scope` under it, each of which runs exactly once when
// reached -- the front end wraps a declaration's initialiser in one at
// -O2, and a store there is as good as one in the block itself -- and the
// load comes after it, at whatever depth. A store in an `if` or a loop
// does not qualify.
static bool storeDominatesLoad(cir::StoreOp st, cir::LoadOp ld, Block *top) {
    llvm::SmallVector<Operation *, 8> chain;   // st, then each scope up to top
    Operation *o = st.getOperation();
    while (o->getBlock() != top) {
        chain.push_back(o);
        Operation *parent = o->getParentOp();
        if (!parent || !mlir::isa<cir::ScopeOp, cir::CleanupScopeOp>(parent)) return false;
        o = parent;
    }
    chain.push_back(o);
    for (int i = chain.size() - 1; i >= 0; --i) {
        Operation *anc = chain[i]->getBlock()->findAncestorOpInBlock(*ld.getOperation());
        if (!anc) return false;
        if (anc != chain[i]) return chain[i]->isBeforeInBlock(anc);
        // The load is inside the same scope as the store: look one level in.
    }
    return false;
}

static Value singleStoredValue(cir::AllocaOp alloca) {
    cir::StoreOp theStore;
    llvm::SmallVector<cir::LoadOp, 8> loads;
    for (Operation *user : alloca->getUsers()) {
        // A lifetime marker names the slot and touches nothing in it.
        if (mlir::isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(user)) continue;
        if (auto st = mlir::dyn_cast<cir::StoreOp>(user)) {
            if (st.getAddr() != alloca.getResult())
                return nullptr;          // the slot's address is the stored value
            // `p = NULL` is no second value: a null reaches no field access,
            // and `T *p = NULL; ... p = &x;` is the commonest shape of a
            // pointer local.
            if (auto c = st.getValue().getDefiningOp<cir::ConstantOp>())
                if (auto cp = mlir::dyn_cast<cir::ConstPtrAttr>(c.getValue()))
                    if (cp.getValue().getValue().isZero()) continue;
            if (theStore)
                return nullptr;          // two stores: not a single assignment
            theStore = st;
            continue;
        }
        if (auto ld = mlir::dyn_cast<cir::LoadOp>(user)) {
            loads.push_back(ld);
            continue;
        }
        return nullptr;                  // address escapes into something else
    }
    if (!theStore)
        return nullptr;
    for (cir::LoadOp ld : loads)
        if (!storeDominatesLoad(theStore, ld, alloca->getBlock()))
            return nullptr;              // a load the store does not dominate
    return theStore.getValue();
}

Value traceToRoot(Value v) {
    while (true) {
        Value local = traceToLocalRoot(v);
        auto load = local.getDefiningOp<cir::LoadOp>();
        if (!load)
            return local;
        auto alloca = load.getAddr().getDefiningOp<cir::AllocaOp>();
        if (!alloca)
            return local;
        Value stored = singleStoredValue(alloca);
        if (!stored)
            return local;
        v = stored;
    }
}


// A stack slot is a different object in every invocation, so two threads
// running the same function do not share it and cannot falsely share its
// fields. Running on real compiler output, this was the whole of the
// pass's noise: libstdc++ locals such as allocated_ptr's __guard_ptr,
// reported because the function they live in is reachable from many
// threads. They are only interesting if the address is published, and
// even then two invocations are two objects, so an escaping slot can
// reach "possible" and never "definite".
bool addressEscapes(cir::AllocaOp slot) {
    llvm::SmallVector<Value, 8> work{slot.getResult()};
    llvm::DenseSet<Operation *> seen;
    unsigned guard = 0;
    while (!work.empty() && guard++ < 512) {
        Value v = work.pop_back_val();
        for (Operation *u : v.getUsers()) {
            // A store is judged per operand: the same store may hold
            // the slot's address as its destination and, through another
            // derived value, as the value stored, and only the second
            // publishes it.
            if (auto st = dyn_cast<cir::StoreOp>(u)) {
                if (st.getValue() == v) return true;
                continue;
            }
            if (!seen.insert(u).second) continue;
            if (isa<cir::LoadOp>(u)) continue;                  // reads the slot
            if (isa<cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;   // names it, touches nothing
            if (isa<cir::CastOp, cir::GetMemberOp, cir::GetElementOp, cir::PtrStrideOp>(u)) {
                work.push_back(u->getResult(0));
                continue;
            }
            return true;    // a call, a global initialiser, anything else
        }
    }
    // Out of budget with work left: assume the worst. With the work
    // done, the address went nowhere. An earlier version returned true
    // on both exits, so no slot was ever private and the summary's
    // "on stack slots that never escape" was always zero.
    return !work.empty();
}

unsigned spawnEntryOperand(cir::CallOp op) {
    cir::FuncOp callee = lookupCallee(op);
    // pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    //                void *(*start)(void *), void *arg)
    if (callee && callee.getSymName() == "pthread_create")
        return firstRealOperand(op) + 2;
    // thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
    if (callee && callee.getSymName() == "thrd_create")
        return firstRealOperand(op) + 1;
    return firstRealOperand(op);
}

unsigned spawnFirstArgOperand(cir::CallOp op) {
    return spawnEntryOperand(op) + 1;
}

bool touchesOnlyItsArguments(cir::FuncOp f) {
    if (!f || !f.getBody().empty()) return false;
    static const llvm::StringSet<> names = {
        "memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strcmp", "strncmp",
        "abs", "labs", "llabs", "sqrt", "sqrtf", "fabs", "fabsf"};
    return names.contains(f.getSymName());
}

bool isAllocator(cir::FuncOp f) {
    if (!f) return false;
    const StringRef n = f.getSymName();
    if (n == "malloc" || n == "calloc" || n == "aligned_alloc" || n == "memalign" ||
        n == "posix_memalign" || n == "_Znwm" || n == "_Znam" ||
        n == "_ZnwmSt11align_val_t" || n == "_ZnamSt11align_val_t")
        return true;
    // DPDK's EAL heap, rte_malloc(type, size, align) and its zeroing, counting
    // and per-socket forms, and the ring, stack and mempool constructors,
    // which hand back a fresh object that exists nowhere else:
    // rte_ring_create(name, count, socket, flags), rte_ring_create_elem(name,
    // esize, count, socket, flags), rte_stack_create(name, count, socket,
    // flags), rte_mempool_create and rte_mempool_create_empty, and the mbuf
    // pools' rte_pktmbuf_pool_create, _by_ops and _extbuf. The memzone
    // behind them is reserved cache-line aligned, which the pass does not
    // read; the heap-align option stands in for it.
    return n == "rte_malloc" || n == "rte_zmalloc" || n == "rte_calloc" ||
           n == "rte_malloc_socket" || n == "rte_zmalloc_socket" || n == "rte_calloc_socket" ||
           n == "rte_ring_create" || n == "rte_ring_create_elem" || n == "rte_stack_create" ||
           n == "rte_mempool_create" || n == "rte_mempool_create_empty" || n == "rte_pktmbuf_pool_create" ||
           n == "rte_pktmbuf_pool_create_by_ops" || n == "rte_pktmbuf_pool_create_extbuf";
}

static void allocationsBehindImpl(cir::CallOp call, unsigned depth, llvm::SmallVectorImpl<cir::CallOp> &sites,
                                  bool &bad);

// One value a wrapper hands back: a null, which reaches no field access; an
// allocation, behind another wrapper or not; or a load from a slot every
// store into which hands back one of those -- the front end's `__retval`,
// which every `return` of a function with several of them stores to.
static void returnedValue(Value v, unsigned depth, llvm::SmallVectorImpl<cir::CallOp> &sites, bool &bad) {
    if (bad) return;
    if (depth > 8) { bad = true; return; }
    Value r = traceToRoot(v);
    if (auto c = r.getDefiningOp<cir::ConstantOp>())
        if (auto cp = dyn_cast<cir::ConstPtrAttr>(c.getValue()))
            if (cp.getValue().getValue().isZero()) return;
    if (auto c = r.getDefiningOp<cir::CallOp>()) { allocationsBehindImpl(c, depth + 1, sites, bad); return; }
    if (auto ld = r.getDefiningOp<cir::LoadOp>()) {
        auto slot = ld.getAddr().getDefiningOp<cir::AllocaOp>();
        if (slot && !addressEscapes(slot)) {
            bool any = false;
            for (Operation *u : slot->getUsers())
                if (auto st = dyn_cast<cir::StoreOp>(u))
                    if (st.getAddr() == slot.getResult()) { any = true; returnedValue(st.getValue(), depth + 1, sites, bad); }
            if (any) return;
        }
    }
    bad = true;
}

static void allocationsBehindImpl(cir::CallOp call, unsigned depth, llvm::SmallVectorImpl<cir::CallOp> &sites,
                                  bool &bad) {
    cir::FuncOp callee = lookupCallee(call);
    if (!callee) { bad = true; return; }
    if (isAllocator(callee)) {
        if (!llvm::is_contained(sites, call)) sites.push_back(call);
        return;
    }
    if (callee.getBody().empty() || depth > 4) { bad = true; return; }
    callee.walk([&](cir::ReturnOp ret) {
        if (bad) return;
        if (ret.getNumOperands() != 1) { bad = true; return; }
        returnedValue(ret.getOperand(0), depth, sites, bad);
    });
}

llvm::SmallVector<cir::CallOp, 4> allocationsBehind(cir::CallOp call) {
    llvm::SmallVector<cir::CallOp, 4> sites;
    bool bad = false;
    allocationsBehindImpl(call, 0, sites, bad);
    if (bad) sites.clear();
    return sites;
}

cir::CallOp allocationBehind(cir::CallOp call) {
    llvm::SmallVector<cir::CallOp, 4> sites = allocationsBehind(call);
    return sites.size() == 1 ? sites.front() : cir::CallOp();
}

std::optional<uint64_t> allocationAlignment(cir::CallOp call) {
    call = allocationBehind(call);
    cir::FuncOp callee = call ? lookupCallee(call) : cir::FuncOp();
    if (!callee) return std::nullopt;
    const StringRef n = callee.getSymName();
    unsigned idx;
    if (n == "aligned_alloc" || n == "memalign") idx = firstRealOperand(call);
    else if (n == "posix_memalign") idx = firstRealOperand(call) + 1;
    else if (n == "_ZnwmSt11align_val_t" || n == "_ZnamSt11align_val_t") idx = firstRealOperand(call) + 1;
    // rte_malloc(type, size, align), rte_calloc(type, num, size, align); an
    // align of 0 means the EAL's default, which is not read here.
    else if (n == "rte_malloc" || n == "rte_zmalloc") idx = firstRealOperand(call) + 2;
    else if (n == "rte_calloc") idx = firstRealOperand(call) + 3;
    else if (n == "rte_malloc_socket" || n == "rte_zmalloc_socket") idx = firstRealOperand(call) + 2;
    else if (n == "rte_calloc_socket") idx = firstRealOperand(call) + 3;
    else return std::nullopt;
    if (idx >= call.getNumOperands()) return std::nullopt;
    if (auto c = call.getOperand(idx).getDefiningOp<cir::ConstantOp>())
        if (auto a = dyn_cast<cir::IntAttr>(c.getValue()))
            if (a.getValue().getActiveBits() <= 32 && a.getValue().getZExtValue() > 0)
                return a.getValue().getZExtValue();
    return std::nullopt;
}

void memoryAccesses(Operation *op, llvm::SmallVectorImpl<MemoryAccess> &out) {
    auto pointee = [](Value ptr) -> Type {
        if (auto pt = dyn_cast<cir::PointerType>(ptr.getType())) return pt.getPointee();
        return Type();
    };
    // A libc memory call touches `len` bytes: an array of that many bytes
    // when the length is a constant, else the pointee, which is at least
    // what the call was written over.
    // Behind the void* the call takes sits the pointer the program wrote,
    // whose pointee is the least the call touches; failing that, one byte.
    auto lengthType = [&](Value len, Value ptr) -> Type {
        if (auto c = len.getDefiningOp<cir::ConstantOp>())
            if (auto n = dyn_cast<cir::IntAttr>(c.getValue()))
                if (n.getValue().getActiveBits() <= 32 && n.getValue().getZExtValue() > 0)
                    return cir::ArrayType::get(op->getContext(),
                                               cir::IntType::get(op->getContext(), 8, false),
                                               n.getValue().getZExtValue());
        Type t = pointee(traceToLocalRoot(ptr));
        if (!t || isa<cir::VoidType>(t) || isa<cir::FuncType>(t))
            t = cir::IntType::get(op->getContext(), 8, false);
        return t;
    };
    if (auto ld = dyn_cast<cir::LoadOp>(op)) {
        out.push_back({ld.getAddr(), ld.getResult().getType(), false});
    } else if (auto st = dyn_cast<cir::StoreOp>(op)) {
        out.push_back({st.getAddr(), st.getValue().getType(), true});
    } else if (auto rmw = dyn_cast<cir::AtomicFetchOp>(op)) {
        out.push_back({rmw.getPtr(), rmw.getVal().getType(), true});
    } else if (auto x = dyn_cast<cir::AtomicXchgOp>(op)) {
        out.push_back({x.getPtr(), x.getVal().getType(), true});
    } else if (auto cx = dyn_cast<cir::AtomicCmpXchgOp>(op)) {
        out.push_back({cx.getPtr(), cx.getDesired().getType(), true});
    } else if (auto ts = dyn_cast<cir::AtomicTestAndSetOp>(op)) {
        out.push_back({ts.getPtr(), pointee(ts.getPtr()), true});
    } else if (auto cl = dyn_cast<cir::AtomicClearOp>(op)) {
        out.push_back({cl.getPtr(), pointee(cl.getPtr()), true});
    } else if (auto sb = dyn_cast<cir::SetBitfieldOp>(op)) {
        // The whole storage unit is rewritten, whatever the field's width.
        out.push_back({sb.getAddr(), pointee(sb.getAddr()), true});
    } else if (auto gb = dyn_cast<cir::GetBitfieldOp>(op)) {
        out.push_back({gb.getAddr(), pointee(gb.getAddr()), false});
    } else if (auto cp = dyn_cast<cir::CopyOp>(op)) {
        out.push_back({cp.getSrc(), pointee(cp.getSrc()), false});
        out.push_back({cp.getDst(), pointee(cp.getDst()), true});
    } else if (auto ms = dyn_cast<cir::MemSetOp>(op)) {
        out.push_back({ms.getDst(), lengthType(ms.getLen(), ms.getDst()), true});
    } else if (auto mc = dyn_cast<cir::MemCpyOp>(op)) {
        out.push_back({mc.getSrc(), lengthType(mc.getLen(), mc.getSrc()), false});
        out.push_back({mc.getDst(), lengthType(mc.getLen(), mc.getDst()), true});
    } else if (auto mm = dyn_cast<cir::MemMoveOp>(op)) {
        out.push_back({mm.getSrc(), lengthType(mm.getLen(), mm.getSrc()), false});
        out.push_back({mm.getDst(), lengthType(mm.getLen(), mm.getDst()), true});
    }
}

llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>>
collectQueueConstructors(ModuleOp module) {
    llvm::DenseMap<Operation *, llvm::SmallVector<cir::CallOp, 2>> ctorsOf;
    module.walk([&](cir::CallOp call) {
        if (getSemanticKind(lookupCallee(call)) != SemanticOpKind::QueueConstruction) return;
        if (call.getNumOperands() == 0 || firstRealOperand(call) != 0) return;
        Value t = traceToRoot(call.getOperand(0));
        Operation *root = nullptr;
        if (auto gg = t.getDefiningOp<cir::GetGlobalOp>())
            root = SymbolTable::lookupNearestSymbolFrom<cir::GlobalOp>(gg, gg.getNameAttr()).getOperation();
        else if (auto a = t.getDefiningOp<cir::AllocaOp>())
            root = a.getOperation();
        if (root) ctorsOf[root].push_back(call);
    });
    return ctorsOf;
}

std::optional<int64_t> constructorTag(llvm::ArrayRef<cir::CallOp> ctors) {
    if (ctors.size() != 1) return std::nullopt;
    cir::CallOp ctor = ctors.front();
    if (ctor.getNumOperands() != 2) return std::nullopt;
    auto c = ctor.getOperand(1).getDefiningOp<cir::ConstantOp>();
    auto v = c ? dyn_cast<cir::IntAttr>(c.getValue()) : cir::IntAttr();
    if (!v) return std::nullopt;
    return v.getSInt();
}

std::optional<CountedLoop> countedLoop(cir::ForOp loop) {
    Region &cond = loop.getCond();
    if (!cond.hasOneBlock()) return std::nullopt;
    auto term = dyn_cast<cir::ConditionOp>(cond.front().getTerminator());
    auto cmp = term ? term.getCondition().getDefiningOp<cir::CmpOp>() : cir::CmpOp();
    if (!cmp || !isa<cir::IntType>(cmp.getLhs().getType())) return std::nullopt;
    auto lhs = cmp.getLhs().getDefiningOp<cir::LoadOp>();
    auto rhs = cmp.getRhs().getDefiningOp<cir::LoadOp>();
    if (!lhs || !rhs) return std::nullopt;
    // i on the left of < or <=, or on the right of > or >=.
    cir::LoadOp ind, bound;
    bool inclusive = false;
    switch (cmp.getKind()) {
    case cir::CmpOpKind::le: inclusive = true; [[fallthrough]];
    case cir::CmpOpKind::lt: ind = lhs; bound = rhs; break;
    case cir::CmpOpKind::ge: inclusive = true; [[fallthrough]];
    case cir::CmpOpKind::gt: ind = rhs; bound = lhs; break;
    default: return std::nullopt;
    }
    auto slot = ind.getAddr().getDefiningOp<cir::AllocaOp>();
    if (!slot) return std::nullopt;
    Value boundAddr = bound.getAddr();
    if (boundAddr == slot.getResult() ||
        !(boundAddr.getDefiningOp<cir::GetGlobalOp>() || boundAddr.getDefiningOp<cir::AllocaOp>()))
        return std::nullopt;
    // i is read and written directly and nowhere else (its lifetime marks
    // at -O1 aside), and inside the loop only the step writes it, once,
    // adding one.
    unsigned steps = 0;
    for (Operation *u : slot->getUsers()) {
        if (isa<cir::LoadOp, cir::LifetimeStartOp, cir::LifetimeEndOp>(u)) continue;
        auto st = dyn_cast<cir::StoreOp>(u);
        if (!st || st.getAddr() != slot.getResult()) return std::nullopt;
        if (!loop->isAncestor(u)) continue;
        if (!loop.getStep().isAncestor(u->getParentRegion())) return std::nullopt;
        auto loadsI = [&](Value x) {
            auto l = x.getDefiningOp<cir::LoadOp>();
            return l && l.getAddr() == slot.getResult();
        };
        bool byOne = false;
        if (auto inc = st.getValue().getDefiningOp<cir::IncOp>()) byOne = loadsI(inc.getInput());
        else if (auto add = st.getValue().getDefiningOp<cir::AddOp>()) {
            auto c = add.getRhs().getDefiningOp<cir::ConstantOp>();
            auto k = c ? dyn_cast<cir::IntAttr>(c.getValue()) : cir::IntAttr();
            byOne = k && k.getSInt() == 1 && loadsI(add.getLhs());
        }
        if (!byOne) return std::nullopt;
        ++steps;
    }
    if (steps != 1) return std::nullopt;
    // Where it starts: the nearest store to i before the loop in its block,
    // of a constant, with nothing in between that writes i.
    for (Operation *op = loop->getPrevNode(); op; op = op->getPrevNode()) {
        if (auto st = dyn_cast<cir::StoreOp>(op); st && st.getAddr() == slot.getResult()) {
            auto c = st.getValue().getDefiningOp<cir::ConstantOp>();
            auto v = c ? dyn_cast<cir::IntAttr>(c.getValue()) : cir::IntAttr();
            if (!v) return std::nullopt;
            return CountedLoop{loop, bound, v.getSInt(), inclusive};
        }
        bool writes = false;
        op->walk([&](cir::StoreOp st) { writes |= st.getAddr() == slot.getResult(); });
        if (writes) return std::nullopt;
    }
    return std::nullopt;
}

bool spscWhenLoopsRunOnce(Operation *root) {
    auto t = root->getAttrOfType<TopologyAttr>(kTopologyAttrName);
    if (!t) return false;
    const bool manyProducers = t.getValue() == Topology::mpsc || t.getValue() == Topology::mpmc;
    const bool manyConsumers = t.getValue() == Topology::spmc || t.getValue() == Topology::mpmc;
    if (t.getValue() == Topology::unknown) return false;
    return (!manyProducers || root->hasAttr(kProducersLoopAttrName)) &&
           (!manyConsumers || root->hasAttr(kConsumersLoopAttrName));
}

} // namespace mlir::wtc
