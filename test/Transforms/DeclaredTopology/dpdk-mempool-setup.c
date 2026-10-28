/* Which pools a free whose pool is not named may reach, and what a pool's
   creation may write.
   - A pool made by rte_mempool_create with no initialiser holds no mbufs
     (NONE): the free taken off a ring does not reach it.
   - The same pool set up for mbufs afterwards, by rte_mempool_obj_iter with
     rte_pktmbuf_init (LATER) or by rte_pktmbuf_pool_init (POOLINIT), may
     hold them.
   - So may a pool made out of sight and only given its ops here
     (ELSEWHERE).
   - rte_mempool_create runs the program's initialisers with the arguments
     it is handed, which may rewrite a ring kept in them (CALLBACK): the
     ring is no longer placed, and its declaration is not verified.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=NONE
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DLATER -o %t.later.cir
   RUN: wtc-opt %t.later.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=LATER
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DPOOLINIT -o %t.poolinit.cir
   RUN: wtc-opt %t.poolinit.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=LATER
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DELSEWHERE -o %t.elsewhere.cir
   RUN: wtc-opt %t.elsewhere.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=ELSEWHERE --implicit-check-not=verified
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DCALLBACK -o %t.callback.cir
   RUN: wtc-opt %t.callback.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CALLBACK --implicit-check-not=error

   NONE: remark: wtc: queue *@objects: declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT), verified in this translation unit
   LATER: warning: wtc: queue *@objects is declared spmc (by its flags RTE_MEMPOOL_F_SP_PUT); cannot verify: an mbuf freed at dpdk-mempool-setup.c:{{[0-9]+}}:{{[0-9]+}} goes back to the pool its `pool` field names
   ELSEWHERE: warning: wtc: queue @outside is declared spmc (by its ops "ring_sp_mc"); cannot verify: an mbuf freed at dpdk-mempool-setup.c:{{[0-9]+}}:{{[0-9]+}} goes back to the pool its `pool` field names
   CALLBACK: remark: wtc: queue access is unresolvable
*/

#include "rte_mempool.h"

struct rte_mempool *objects;
struct rte_ring *ring;
#ifdef ELSEWHERE
extern struct rte_mempool outside;
#define POOL (&outside)
#else
#define POOL objects
#endif

#ifdef CALLBACK
struct conf {
    struct rte_ring *r;
} conf;
void set_up(struct rte_mempool *mp, void *arg, void *obj, unsigned i);
#endif

static int putter(void *arg) {
#ifdef CALLBACK
    rte_ring_enqueue(conf.r, 0);
#else
    void *o = 0;
    rte_mempool_put(POOL, o);
#endif
    return 0;
}

static int drain(void *arg) {
#ifdef CALLBACK
    rte_ring_enqueue(conf.r, 0);
#else
    void *m;
    rte_ring_dequeue(ring, &m);
    rte_pktmbuf_free((struct rte_mbuf *)m);
#endif
    return 0;
}

int main(void) {
    ring = rte_ring_create("r", 1024, SOCKET_ID_ANY, 0);
#if defined(ELSEWHERE)
    rte_mempool_set_ops_byname(&outside, "ring_sp_mc", NULL);
#elif defined(CALLBACK)
    conf.r = rte_ring_create("c", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    objects = rte_mempool_create("x", 1023, 64, 0, 0, NULL, NULL, set_up, &conf, SOCKET_ID_ANY, 0);
#else
    objects = rte_mempool_create("x", 1023, 2048, 0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY, RTE_MEMPOOL_F_SP_PUT);
#if defined(LATER)
    rte_mempool_obj_iter(objects, rte_pktmbuf_init, NULL);
#elif defined(POOLINIT)
    rte_pktmbuf_pool_init(objects, NULL);
#endif
#endif
    rte_eal_remote_launch(putter, 0, 1);
    rte_eal_remote_launch(drain, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
