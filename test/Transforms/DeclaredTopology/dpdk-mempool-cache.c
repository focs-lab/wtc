/* A pool with per-lcore caches (cache_size > 0). A put goes to the calling
   lcore's cache, and the lcore flushes the cache into the pool's ring when
   it fills; a get refills it the same way. The cache changes when the ring
   is touched, not which threads may touch it: every lcore that puts may
   flush, so two lcores that put break a single-producer pool however
   large the cache. A thread with no lcore (a pthread the EAL does not
   know) has no cache and touches the ring at once. A cache the program
   owns is flushed by the thread that calls rte_mempool_cache_flush. So
   every put and get counts on the thread that makes it.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s

   CHECK-DAG: error: wtc: queue *@cached is declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET) but 2 threads push to it
   CHECK-DAG: error: wtc: queue *@no_lcore is declared mpsc (by its flags RTE_MEMPOOL_F_SC_GET) but 2 threads pop from it
   CHECK-DAG: note: pop on a thread that starts in outside at
   CHECK-DAG: remark: wtc: queue *@owned: declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET), verified in this translation unit
*/

#include "rte_mempool.h"

typedef unsigned long pthread_t;
int pthread_create(pthread_t *thread, const void *attr, void *(*start)(void *), void *arg);

struct rte_mempool *cached, *no_lcore, *owned;
struct rte_mempool_cache *mine;

static int a(void *arg) {
    void *objs[8];
    rte_mempool_put_bulk(cached, objs, 8);
    rte_mempool_get(no_lcore, &objs[0]);
    rte_mempool_generic_put(owned, objs, 8, mine);
    rte_mempool_cache_flush(mine, owned);
    return 0;
}

static int b(void *arg) {
    void *o;
    rte_mempool_put(cached, 0);
    rte_mempool_get(cached, &o);
    rte_mempool_get(owned, &o);
    return 0;
}

static void *outside(void *arg) {
    void *o;
    rte_mempool_get(no_lcore, &o);
    return 0;
}

int main(void) {
    cached = rte_mempool_create("c", 8191, 64, 256, 0, 0, 0, 0, 0, SOCKET_ID_ANY,
                                RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    no_lcore = rte_mempool_create("n", 8191, 64, 256, 0, 0, 0, 0, 0, SOCKET_ID_ANY, RTE_MEMPOOL_F_SC_GET);
    owned = rte_mempool_create("o", 8191, 64, 0, 0, 0, 0, 0, 0, SOCKET_ID_ANY,
                               RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    mine = rte_mempool_cache_create(32, SOCKET_ID_ANY);
    pthread_t t;
    pthread_create(&t, 0, outside, 0);
    rte_eal_remote_launch(a, 0, 1);
    rte_eal_remote_launch(b, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
