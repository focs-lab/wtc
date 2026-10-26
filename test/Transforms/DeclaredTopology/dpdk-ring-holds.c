/* A DPDK ring created single-producer single-consumer by its flags, with
   one lcore enqueuing and another dequeuing: the declaration holds. The
   default rte_ring_enqueue dispatches to rte_ring_sp_enqueue_bulk_elem
   inside the library, which is the library's choice and no claim of the
   program's.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s

   CHECK: remark: wtc: queue *@ring: spsc (producers: 1, consumers: 1)
   CHECK: remark: wtc: queue *@ring: declared spsc (by its flags RING_F_SP_ENQ | RING_F_SC_DEQ), verified in this translation unit{{$}}
   CHECK-NOT: error
*/

#include "rte_ring.h"

struct rte_ring *ring;

static int producer(void *arg) {
    for (long i = 0; i < 100; i++) rte_ring_enqueue(ring, (void *)i);
    return 0;
}

static int consumer(void *arg) {
    void *obj;
    for (int got = 0; got < 100;) if (rte_ring_dequeue(ring, &obj) == 0) got++;
    return 0;
}

int main(void) {
    ring = rte_ring_create("rx_to_tx", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    rte_eal_remote_launch(producer, 0, 1);
    rte_eal_remote_launch(consumer, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
