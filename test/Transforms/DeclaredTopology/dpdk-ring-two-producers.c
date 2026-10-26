/* A ring whose flags promise one producer, enqueued to from two lcores:
   the build stops and names both. The ring travels as the lcore's
   argument, as DPDK's examples pass it.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s

   CHECK: error: wtc: queue {{.*}} is declared spmc (by its flags) but 2 threads push to it
   CHECK-DAG: note: push on a thread that starts in rx_a at {{.*}}dpdk-ring-two-producers.c:35
   CHECK-DAG: note: push on a thread that starts in rx_b at {{.*}}dpdk-ring-two-producers.c:36
*/

#include "rte_ring.h"

static int rx_a(void *arg) {
    struct rte_ring *r = arg;
    rte_ring_enqueue(r, 0);
    return 0;
}

static int rx_b(void *arg) {
    struct rte_ring *r = arg;
    rte_ring_enqueue(r, 0);
    return 0;
}

static int worker(void *arg) {
    void *obj;
    rte_ring_dequeue((struct rte_ring *)arg, &obj);
    return 0;
}

int main(void) {
    struct rte_ring *r = rte_ring_create("rx", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ);
    rte_eal_remote_launch(rx_a, r, 1);
    rte_eal_remote_launch(rx_b, r, 2);
    rte_eal_remote_launch(worker, r, 3);
    rte_eal_mp_wait_lcore();
    return 0;
}
