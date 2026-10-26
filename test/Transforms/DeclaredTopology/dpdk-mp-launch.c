/* rte_eal_mp_remote_launch starts its function on every worker lcore, so a
   ring those workers dequeue from has many consumers, and a single-consumer
   flag on it is broken. With CALL_MAIN the calling lcore runs the function
   too, and is named among the threads; with SKIP_MAIN it is not.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>%t.out
   RUN: FileCheck %s < %t.out
   RUN: grep -A12 'queue \*@with_main is declared' %t.out | FileCheck %s --check-prefix=CALL
   RUN: sed -n '/queue \*@workers_only is declared/,/queue \*@with_main/p' %t.out | FileCheck %s --check-prefix=SKIP

   CHECK-DAG: remark: wtc: queue *@workers_only: many consumers: started on every worker lcore at dpdk-mp-launch.c:{{[0-9]+}}:{{[0-9]+}}
   CHECK-DAG: error: wtc: queue *@workers_only is declared spsc (by its flags RING_F_SP_ENQ | RING_F_SC_DEQ) but many threads pop from it
   CHECK-DAG: error: wtc: queue *@with_main is declared spsc (by its flags RING_F_SP_ENQ | RING_F_SC_DEQ) but many threads pop from it
   CHECK-DAG: remark: wtc: queue *@from_main: declared spmc (by its flags RING_F_SP_ENQ), verified in this translation unit

   CALL: note: pop on a thread that starts in main (the program's own thread)
   SKIP: note: pop on a thread that starts in skip_worker, one of several started
   SKIP-NOT: (the program's own thread)
*/

#include "rte_ring.h"

struct rte_ring *workers_only, *with_main, *from_main;

static int skip_worker(void *arg) {
    void *obj;
    rte_ring_dequeue(workers_only, &obj);
    rte_ring_dequeue(from_main, &obj);
    return 0;
}

static int call_worker(void *arg) {
    void *obj;
    rte_ring_dequeue(with_main, &obj);
    return 0;
}

int main(void) {
    workers_only = rte_ring_create("w", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    with_main = rte_ring_create("c", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    from_main = rte_ring_create("m", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ);
    rte_ring_enqueue(workers_only, 0);
    rte_ring_enqueue(with_main, 0);
    rte_ring_enqueue(from_main, 0);
    rte_eal_mp_remote_launch(skip_worker, 0, SKIP_MAIN);
    rte_eal_mp_wait_lcore();
    rte_eal_mp_remote_launch(call_worker, 0, CALL_MAIN);
    rte_eal_mp_wait_lcore();
    return 0;
}
