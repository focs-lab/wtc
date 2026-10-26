/* A ring created with no flags, so many on both sides, but enqueued to
   with rte_ring_sp_enqueue, which promises one producer at that call:
   with two lcores calling it, that promise is broken. Through the default
   rte_ring_enqueue, whose body may take the same single-producer path,
   nothing is promised, and a second ring used that way is only reported
   as more cautious than its threads need.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s

   CHECK-DAG: error: wtc: queue *@explicit_sp is declared spmc (by rte_ring_sp_enqueue at dpdk-ring-explicit-sp.c:{{[0-9]+}}:{{[0-9]+}}) but 2 threads push to it
   CHECK-DAG: remark: wtc: queue *@by_default is declared mpmc, but in this translation unit its threads allow mpsc
   CHECK-NOT: queue *@by_default is declared spmc
*/

#include "rte_ring.h"

struct rte_ring *explicit_sp, *by_default;

static int rx_a(void *arg) {
    rte_ring_sp_enqueue(explicit_sp, 0);
    rte_ring_enqueue(by_default, 0);
    return 0;
}

static int rx_b(void *arg) {
    rte_ring_sp_enqueue(explicit_sp, 0);
    rte_ring_enqueue(by_default, 0);
    return 0;
}

static int tx(void *arg) {
    void *obj;
    rte_ring_dequeue(explicit_sp, &obj);
    rte_ring_dequeue(by_default, &obj);
    return 0;
}

int main(void) {
    explicit_sp = rte_ring_create("a", 1024, SOCKET_ID_ANY, 0);
    by_default = rte_ring_create("b", 1024, SOCKET_ID_ANY, 0);
    rte_eal_remote_launch(rx_a, 0, 1);
    rte_eal_remote_launch(rx_b, 0, 2);
    rte_eal_remote_launch(tx, 0, 3);
    rte_eal_mp_wait_lcore();
    return 0;
}
