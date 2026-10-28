/* What keeps a free's pool from being named, and code out of sight that
   may put or get. A free walks the mbuf's chain and the buffer an indirect
   mbuf is attached to, each into its own pool: once the program links
   mbufs -- rte_pktmbuf_chain (CHAIN), or its own `m->next` (NEXT) -- a
   free is not followed to one pool, while rte_mbuf_raw_free, one direct
   segment, still is. A free whose address is taken (TAKEN) is called
   where no call site shows, so every pool is unknown: none is verified,
   though two threads seen putting still break one, and a pool on the
   default ops, which declares nothing single, has nothing to check and
   gets no warning. Mbufs handed to a driver's burst function (TX) may be
   freed there, on the thread that hands them over: the verdict holds in
   this translation unit and names it.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DCHAIN -o %t.chain.cir
   RUN: not wtc-opt %t.chain.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CHAIN
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DNEXT -o %t.next.cir
   RUN: not wtc-opt %t.next.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CHAIN
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DTAKEN -o %t.taken.cir
   RUN: not wtc-opt %t.taken.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAKEN --implicit-check-not='@spare cannot be checked' --implicit-check-not=verified
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DTX -o %t.tx.cir
   RUN: not wtc-opt %t.tx.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=TX

   CHECK-DAG: remark: wtc: queue *@pool: declared spmc (by its ops "ring_sp_mc"), verified in this translation unit
   CHECK-DAG: error: wtc: queue *@raw is declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT) but 2 threads push to it
   CHAIN-DAG: warning: wtc: queue *@pool is declared spmc (by its ops "ring_sp_mc"); cannot verify: an mbuf freed at dpdk-mempool-links.c:{{[0-9]+}}:{{[0-9]+}} may have segments or an attached buffer from other pools, since mbufs are linked at dpdk-mempool-links.c:{{[0-9]+}}:{{[0-9]+}}
   CHAIN-DAG: error: wtc: queue *@raw is declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT) but 2 threads push to it
   TAKEN-DAG: remark: wtc: the address of rte_pktmbuf_free is taken, and calls through it are not counted; every queue of its type is now unknown
   TAKEN-DAG: warning: wtc: queue *@pool is declared spmc (by its ops "ring_sp_mc"); cannot verify: the address of rte_pktmbuf_free is taken, and calls through it are not counted
   TAKEN-DAG: error: wtc: queue *@raw is declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT) but 2 threads push to it
   TX-DAG: remark: wtc: queue *@pool: declared spmc (by its ops "ring_sp_mc"), verified in this translation unit; mbufs are handed to a call through a pointer in rte_eth_tx_burst, which may free them or take more from a pool
   TX-DAG: error: wtc: queue *@raw is declared spmc
*/

#include "rte_mempool.h"

struct rte_mempool *pool, *spare, *raw;

#ifdef TAKEN
void (*release)(struct rte_mbuf *) = rte_pktmbuf_free;
#endif

/* The only thread that puts into `pool`, which it frees into. */
static int worker(void *arg) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    struct rte_mbuf *t = rte_pktmbuf_alloc(pool);
#if defined(CHAIN)
    rte_pktmbuf_chain(m, t);
#elif defined(NEXT)
    m->next = t;
#elif defined(TX)
    rte_eth_tx_burst(0, 0, &t, 1);
#endif
    rte_pktmbuf_free(m);
    struct rte_mbuf *r = rte_pktmbuf_alloc(raw);
    rte_mbuf_raw_free(r);
    return 0;
}

/* Frees into another pool; puts one direct segment into `raw`, as the
   worker does, which no link can redirect. */
static int other(void *arg) {
    struct rte_mbuf *n = rte_pktmbuf_alloc(spare);
    rte_pktmbuf_free(n);
    struct rte_mbuf *r = rte_pktmbuf_alloc(raw);
    rte_mbuf_raw_free(r);
    return 0;
}

int main(void) {
    pool = rte_pktmbuf_pool_create_by_ops("p", 1023, 32, 0, 2048, SOCKET_ID_ANY, "ring_sp_mc");
    spare = rte_pktmbuf_pool_create("s", 1023, 32, 0, 2048, SOCKET_ID_ANY);
    raw = rte_mempool_create("r", 1023, 2048, 32, 0, rte_pktmbuf_pool_init, 0, rte_pktmbuf_init, 0,
                             SOCKET_ID_ANY, RTE_MEMPOOL_F_SP_PUT);
    rte_eal_remote_launch(worker, 0, 1);
    rte_eal_remote_launch(other, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
