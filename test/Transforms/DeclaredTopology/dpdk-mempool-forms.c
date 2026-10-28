/* The ways a mempool is made and declared, read as DPDK 26.11 does. The
   flags of rte_mempool_create and rte_mempool_create_empty pick the ops
   (RTE_MEMPOOL_F_SP_PUT and RTE_MEMPOOL_F_SC_GET, or the older names with
   the same values); rte_pktmbuf_pool_create_by_ops names them, and
   rte_mempool_set_ops_byname replaces whatever the creation picked. Every
   put and get follows the ops: rte_mempool_put and _get, their bulk and
   generic forms, rte_mempool_cache_flush, and the mbuf allocations from a
   pool. rte_pktmbuf_pool_create takes the default ops, which the program
   does not choose and which declare nothing single.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s

   CHECK-DAG: remark: wtc: queue *@both: declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET), verified in this translation unit
   CHECK-DAG: error: wtc: queue *@empty_sc is declared mpsc (by its flags RTE_MEMPOOL_F_SC_GET) but 2 threads pop from it
   CHECK-DAG: error: wtc: queue *@named is declared spmc (by its ops "ring_sp_mc") but 2 threads push to it
   CHECK-DAG: error: wtc: queue *@by_ops is declared mpsc (by its ops "ring_mp_sc") but 2 threads pop from it
   CHECK-DAG: remark: wtc: queue *@old_names: declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT), verified in this translation unit
   CHECK-DAG: remark: wtc: queue *@flushed: declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET), verified in this translation unit
   CHECK-DAG: remark: wtc: queue *@pktmbuf is declared mpmc, but in this translation unit its threads allow spsc
   CHECK-DAG: remark: wtc: queue *@renamed is declared mpmc, but in this translation unit its threads allow mpsc
*/

#include "rte_mempool.h"

struct rte_mempool *both, *empty_sc, *named, *by_ops, *old_names, *flushed, *pktmbuf, *renamed;
struct rte_mempool_cache *mine;

static int a(void *arg) {
    void *o;
    void *objs[4];
    rte_mempool_put(both, 0);
    rte_mempool_get(empty_sc, &o);
    rte_mempool_put_bulk(named, objs, 4);
    rte_pktmbuf_alloc(by_ops);
    rte_mempool_put(old_names, 0);
    rte_mempool_generic_put(flushed, objs, 4, mine);
    rte_mempool_cache_flush(mine, flushed);
    rte_mempool_put(pktmbuf, 0);
    rte_mempool_put(renamed, 0);
    return 0;
}

static int b(void *arg) {
    void *o;
    void *objs[4];
    rte_mempool_get(both, &o);
    rte_mempool_get_bulk(empty_sc, objs, 4);
    rte_mempool_generic_put(named, objs, 4, 0);
    struct rte_mbuf *ms[4];
    rte_pktmbuf_alloc_bulk(by_ops, ms, 4);
    rte_mempool_get(old_names, &o);
    rte_mempool_get(old_names, &o);
    rte_mempool_generic_get(flushed, objs, 4, 0);
    rte_mempool_get(pktmbuf, &o);
    rte_mempool_put(renamed, 0);
    return 0;
}

static int c(void *arg) {
    void *o;
    rte_mempool_get(old_names, &o);
    return 0;
}

int main(void) {
    both = rte_mempool_create("b", 1023, 64, 0, 0, 0, 0, 0, 0, SOCKET_ID_ANY,
                              RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    empty_sc = rte_mempool_create_empty("e", 1023, 64, 32, 0, SOCKET_ID_ANY, RTE_MEMPOOL_F_SC_GET);
    named = rte_mempool_create_empty("n", 1023, 64, 32, 0, SOCKET_ID_ANY, 0);
    rte_mempool_set_ops_byname(named, "ring_sp_mc", 0);
    by_ops = rte_pktmbuf_pool_create_by_ops("o", 1023, 32, 0, 2048, SOCKET_ID_ANY, "ring_mp_sc");
    old_names = rte_mempool_create("m", 1023, 64, 0, 0, 0, 0, 0, 0, SOCKET_ID_ANY, MEMPOOL_F_SP_PUT);
    flushed = rte_mempool_create("f", 1023, 64, 0, 0, 0, 0, 0, 0, SOCKET_ID_ANY,
                                 MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET | MEMPOOL_F_NO_CACHE_ALIGN);
    mine = rte_mempool_cache_create(32, SOCKET_ID_ANY);
    pktmbuf = rte_pktmbuf_pool_create("p", 1023, 32, 0, 2048, SOCKET_ID_ANY);
    renamed = rte_mempool_create_empty("r", 1023, 64, 32, 0, SOCKET_ID_ANY,
                                       RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    rte_mempool_set_ops_byname(renamed, "ring_mp_mc", 0);
    rte_eal_remote_launch(a, 0, 1);
    rte_eal_remote_launch(b, 0, 2);
    rte_eal_remote_launch(c, 0, 3);
    rte_eal_mp_wait_lcore();
    return 0;
}
