/* A ring's declaration is reported from its flags as written, also on a
   side where no access is seen: a ring whose producer is in another file
   or process is still declared single there by RING_F_SP_ENQ, and the
   report says that no producer was seen rather than calling the ring
   mpsc. A side the flags leave free stays many, seen or not. A side
   declared single with no access here is vacuously satisfied, not
   verified: nothing in this translation unit was checked on it.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s

   CHECK-DAG: remark: wtc: queue *@one_consumer: declared spsc (by its flags RING_F_SP_ENQ | RING_F_SC_DEQ; no producer access seen), verified for its consumer and vacuously satisfied: no producer access in this translation unit
   CHECK-DAG: error: wtc: queue *@two_consumers is declared spsc (by its flags RING_F_SP_ENQ | RING_F_SC_DEQ; no producer access seen) but 2 threads pop from it
   CHECK-DAG: remark: wtc: queue *@no_consumer: declared spmc (by its flags RING_F_SP_ENQ), verified in this translation unit
   CHECK-NOT: declared mpsc
*/

#include "rte_ring.h"

struct rte_ring *one_consumer, *two_consumers, *no_consumer;

static int a(void *arg) {
    void *o;
    rte_ring_dequeue(one_consumer, &o);
    rte_ring_dequeue(two_consumers, &o);
    rte_ring_enqueue(no_consumer, 0);
    return 0;
}

static int b(void *arg) {
    void *o;
    rte_ring_dequeue(two_consumers, &o);
    return 0;
}

int main(void) {
    one_consumer = rte_ring_create("o", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    two_consumers = rte_ring_create("t", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    no_consumer = rte_ring_create("n", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ);
    rte_eal_remote_launch(a, 0, 1);
    rte_eal_remote_launch(b, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
