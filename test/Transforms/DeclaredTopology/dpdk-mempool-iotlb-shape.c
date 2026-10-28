/* The shape of vhost's IOTLB pool before DPDK fixed its declaration
   (46d3f57537, lib/librte_vhost/iotlb.c), with the two threads that break
   it brought into sight: a forwarding lcore that takes an entry on a miss
   and a control thread that takes one on an IOTLB update, both from a pool
   created RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET and kept in a field
   of the virtqueue. With the field written once, the check stops the
   build. vhost writes it twice (LOOKUP): first with what
   rte_mempool_lookup finds under the pool's name, which it frees, then
   with the new pool; a load of the field may then be either, and the
   pool's accesses are unresolved. In vhost itself neither thread is in
   iotlb.c, so the file gives no thread to count in any case.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DLOOKUP -o %t.lookup.cir
   RUN: wtc-opt %t.lookup.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOOKUP

   CHECK: error: wtc: queue the heap object made at dpdk-mempool-iotlb-shape.c:{{[0-9]+}}:{{[0-9]+}} is declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET; no producer access seen) but 2 threads pop from it
   LOOKUP: remark: wtc: queue access is unresolvable (the queue object could not be identified); every queue of this type is now unknown
   LOOKUP-NOT: verified
   LOOKUP-NOT: error
*/

#include "rte_mempool.h"

struct vhost_virtqueue {
    struct rte_mempool *iotlb_pool;
};
struct vhost_virtqueue vq;

static void iotlb_init(struct vhost_virtqueue *q) {
#ifdef LOOKUP
    q->iotlb_pool = rte_mempool_lookup("iotlb_0");
    if (q->iotlb_pool) rte_mempool_free(q->iotlb_pool);
#endif
    q->iotlb_pool = rte_mempool_create("iotlb_0", 2048, 64, 0, 0, NULL, NULL, NULL, NULL, SOCKET_ID_ANY,
                                       MEMPOOL_F_NO_CACHE_ALIGN | MEMPOOL_F_SP_PUT | MEMPOOL_F_SC_GET);
}

static void pending_insert(struct vhost_virtqueue *q) {
    void *node;
    rte_mempool_get(q->iotlb_pool, &node);
}

static void cache_insert(struct vhost_virtqueue *q) {
    void *node;
    rte_mempool_get(q->iotlb_pool, &node);
}

static int forwarding(void *arg) {
    pending_insert(&vq);
    return 0;
}

static int control(void *arg) {
    cache_insert(&vq);
    return 0;
}

int main(void) {
    iotlb_init(&vq);
    rte_eal_remote_launch(forwarding, 0, 1);
    rte_eal_remote_launch(control, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
