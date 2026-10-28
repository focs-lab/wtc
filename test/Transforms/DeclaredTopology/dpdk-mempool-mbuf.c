/* A free puts the mbuf into the pool its `pool` field names. The analysis
   names that pool when the mbuf is, in the function that frees it, what an
   allocation from a pool handed back; then the free is a put into that
   pool, and two threads that each allocate and free break a single-
   producer pool. Any other mbuf -- one taken from a ring here (RING) --
   goes back to a pool the analysis does not name: a put, on its thread,
   into every pool that may hold mbufs. That can make a put side many,
   never proves it, and leaves the get side alone; a pool made by
   rte_mempool_create with no initialiser holds no mbufs and is not
   touched.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DRING -o %t.ring.cir
   RUN: not wtc-opt %t.ring.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=RING

   CHECK-DAG: remark: wtc: queue *@own: declared spsc (by its ops "ring_sp_sc"), verified in this translation unit
   CHECK-DAG: error: wtc: queue *@shared is declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET) but 2 threads push to it
   CHECK-DAG: note: push on a thread that starts in alloc_free at
   CHECK-DAG: note: push on a thread that starts in alloc_free_b at
   CHECK-DAG: remark: wtc: queue *@objects: declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET), verified in this translation unit

   RING-DAG: warning: wtc: queue *@own is declared spsc (by its ops "ring_sp_sc"); cannot verify: an mbuf freed at dpdk-mempool-mbuf.c:{{[0-9]+}}:{{[0-9]+}} goes back to the pool its `pool` field names, which the analysis does not resolve
   RING-DAG: warning: wtc: queue *@rx is declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT); cannot verify: an mbuf freed at dpdk-mempool-mbuf.c:{{[0-9]+}}:{{[0-9]+}} goes back to the pool its `pool` field names, which the analysis does not resolve
   RING-DAG: remark: wtc: queue *@gets: declared mpsc (by its ops "ring_mp_sc"), verified in this translation unit
   RING-DAG: remark: wtc: queue *@objects: declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET), verified in this translation unit
   RING-DAG: error: wtc: queue *@shared is declared spsc
   RING-NOT: queue *@rx: declared
   RING-NOT: queue *@own: declared
*/

#include "rte_mempool.h"

struct rte_mempool *own, *shared, *rx, *gets, *objects;
struct rte_ring *ring;

/* Allocates from `own` and frees what it allocated; the same with
   `shared`, which a second thread also uses. */
static int alloc_free(void *arg) {
    struct rte_mbuf *m = rte_pktmbuf_alloc(own);
    rte_pktmbuf_free(m);
    struct rte_mbuf *s = rte_pktmbuf_alloc(shared);
    rte_pktmbuf_free(s);
    struct rte_mbuf *r = rte_mbuf_raw_alloc(rx);
    rte_ring_enqueue(ring, r);
    void *o;
    rte_mempool_get(objects, &o);
    rte_mempool_put(objects, o);
    return 0;
}

static int alloc_free_b(void *arg) {
    struct rte_mbuf *s = rte_pktmbuf_alloc(shared);
    rte_mbuf_raw_free(s);
    return 0;
}

#ifdef RING
/* Frees mbufs taken from a ring, one by one and as an array: their pool
   is not named. */
static int drain(void *arg) {
    void *m;
    rte_ring_dequeue(ring, &m);
    rte_pktmbuf_free((struct rte_mbuf *)m);
    rte_mempool_get(gets, &m);
    return 0;
}

static int drain_b(void *arg) {
    struct rte_mbuf *ms[1];
    rte_ring_dequeue(ring, (void **)&ms[0]);
    rte_pktmbuf_free_bulk(ms, 1);
    return 0;
}
#endif

int main(void) {
    own = rte_pktmbuf_pool_create_by_ops("own", 1023, 0, 0, 2048, SOCKET_ID_ANY, "ring_sp_sc");
    shared = rte_mempool_create("s", 1023, 2048, 0, 0, rte_pktmbuf_pool_init, 0, rte_pktmbuf_init, 0,
                                SOCKET_ID_ANY, RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    rx = rte_mempool_create_empty("rx", 1023, 2048, 32, 0, SOCKET_ID_ANY, RTE_MEMPOOL_F_SP_PUT);
    gets = rte_pktmbuf_pool_create_by_ops("g", 1023, 0, 0, 2048, SOCKET_ID_ANY, "ring_mp_sc");
    objects = rte_mempool_create("x", 1023, 64, 0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY,
                                 RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    ring = rte_ring_create("r", 1024, SOCKET_ID_ANY, 0);
    rte_eal_remote_launch(alloc_free, 0, 1);
    rte_eal_remote_launch(alloc_free_b, 0, 2);
#ifdef RING
    rte_eal_remote_launch(drain, 0, 3);
    rte_eal_remote_launch(drain_b, 0, 4);
#endif
    rte_eal_mp_wait_lcore();
    return 0;
}
