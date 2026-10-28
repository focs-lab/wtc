/* The rest of the mempool and mbuf API the model reads, and what it does
   with a function of those libraries it does not read.
   - rte_pktmbuf_linearize frees the segments it copies out of: a free
     (LINEARIZE).
   - Populating a pool puts its objects in, as its creation does: part of
     building it, not a producer (POPULATE).
   - The proposed zero-copy cache access reaches the ring as a flush or a
     refill does (ZC).
   - rte_mempool_sp_put and rte_mempool_sc_get, deprecated in DPDK 17.05,
     declare their side single at the call; rte_mempool_mp_put follows the
     ops, and two threads calling it on a flagless pool break nothing
     (OLD).
   - Any other function of the libraries that is handed a pool or an mbuf
     may put or get: every pool is unknown, and no declaration is verified
     (UNKNOWN). One known to neither put nor get, rte_pktmbuf_adj, leaves
     the check as it was (INERT).

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DLINEARIZE -o %t.lin.cir
   RUN: not wtc-opt %t.lin.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=LINEARIZE
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DPOPULATE -o %t.pop.cir
   RUN: wtc-opt %t.pop.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=POPULATE
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DZC -o %t.zc.cir
   RUN: not wtc-opt %t.zc.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=ZC
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DOLD -o %t.old.cir
   RUN: not wtc-opt %t.old.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=OLD
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DUNKNOWN -o %t.unknown.cir
   RUN: wtc-opt %t.unknown.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=UNKNOWN --implicit-check-not=verified
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DINERT -o %t.inert.cir
   RUN: wtc-opt %t.inert.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=INERT

   LINEARIZE: error: wtc: queue *@pool is declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT) but 2 threads push to it
   POPULATE: remark: wtc: queue *@pool: declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT), verified in this translation unit
   ZC-DAG: error: wtc: queue *@pool is declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET; no consumer access seen) but 2 threads push to it
   ZC-DAG: error: wtc: queue *@pool2 is declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET; no producer access seen) but 2 threads pop from it
   OLD-DAG: error: wtc: queue *@pool is declared spmc (by rte_mempool_sp_put at dpdk-mempool-more-forms.c:{{[0-9]+}}:{{[0-9]+}}) but 2 threads push to it
   OLD-DAG: remark: wtc: queue *@pool2: declared mpsc (by rte_mempool_sc_get at dpdk-mempool-more-forms.c:{{[0-9]+}}:{{[0-9]+}}), verified in this translation unit
   UNKNOWN: remark: wtc: rte_pktmbuf_refill is handed an mbuf, and the model does not read what it puts or gets; every queue of its type is now unknown
   UNKNOWN: warning: wtc: queue *@pool is declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT); cannot verify: rte_pktmbuf_refill is handed an mbuf, and the model does not read what it puts or gets
   INERT: remark: wtc: queue *@pool: declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT), verified in this translation unit
*/

#include "rte_mempool.h"

struct rte_mempool *pool, *pool2;
struct rte_mempool_cache *mine;
/* A function of the mbuf library the model does not read. */
int rte_pktmbuf_refill(struct rte_mbuf *m);

static int a(void *arg) {
    void *o;
#if defined(LINEARIZE)
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    rte_pktmbuf_linearize(m);
#elif defined(POPULATE) || defined(INERT) || defined(UNKNOWN)
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
#if defined(INERT)
    rte_pktmbuf_adj(m, 4);
#elif defined(UNKNOWN)
    rte_pktmbuf_refill(m);
#endif
    rte_mbuf_raw_free(m);
#elif defined(ZC)
    rte_mempool_cache_zc_put_bulk(mine, pool, 4);
    rte_mempool_cache_zc_get_bulk(mine, pool2, 4);
#elif defined(OLD)
    rte_mempool_sp_put(pool, 0);
    rte_mempool_mp_put(pool2, 0);
#endif
    return 0;
}

static int b(void *arg) {
    void *o;
#if defined(LINEARIZE)
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    rte_pktmbuf_linearize(m);
#elif defined(ZC)
    rte_mempool_cache_zc_put_bulk(mine, pool, 4);
    rte_mempool_cache_zc_get_bulk(mine, pool2, 4);
#elif defined(OLD)
    rte_mempool_sp_put(pool, 0);
    rte_mempool_mp_put(pool2, 0);
    rte_mempool_sc_get(pool2, &o);
#else
    rte_mempool_get(pool, &o);
#endif
    return 0;
}

int main(void) {
#if defined(ZC)
    pool = rte_mempool_create("p", 1023, 64, 0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY,
                              RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    pool2 = rte_mempool_create("q", 1023, 64, 0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY,
                               RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    mine = rte_mempool_cache_create(32, SOCKET_ID_ANY);
#elif defined(OLD)
    pool = rte_mempool_create("p", 1023, 64, 0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
    pool2 = rte_mempool_create("q", 1023, 64, 0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, 0);
#else
    pool = rte_mempool_create_empty("p", 1023, 2048, 32, 0, SOCKET_ID_ANY, RTE_MEMPOOL_F_SP_PUT);
    rte_mempool_populate_default(pool);
#endif
    rte_eal_remote_launch(a, 0, 1);
    rte_eal_remote_launch(b, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
