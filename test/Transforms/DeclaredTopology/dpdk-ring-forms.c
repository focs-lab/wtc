/* The other ways a ring is made and used. Flags reach the check through
   rte_ring_create_elem's fifth operand, rte_ring_init's fourth, and a
   local holding them. rte_ring_sc_dequeue promises one consumer at the
   call, whatever the flags. A ring flagged single on a side every one of
   whose calls is the explicit multi-thread form, rte_ring_mp_enqueue, is
   not single there: the flag binds only the calls that follow it.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s

   CHECK-DAG: remark: wtc: queue *@elem: declared spsc (by its flags), verified in this translation unit
   CHECK-DAG: error: wtc: queue @init_ring is declared mpsc (by its flags) but 2 threads pop from it
   CHECK-DAG: remark: wtc: queue *@local_flags: declared spsc (by its flags), verified in this translation unit
   CHECK-DAG: error: wtc: queue *@sc_calls is declared mpsc (by rte_ring_sc_dequeue at dpdk-ring-forms.c:{{[0-9]+}}:{{[0-9]+}}) but 2 threads pop from it
   CHECK-DAG: remark: wtc: queue *@mp_calls: declared mpsc (by its flags), verified in this translation unit
   CHECK-NOT: queue *@mp_calls is declared spsc
*/

#include "rte_ring.h"

struct rte_ring *elem, *local_flags, *sc_calls, *mp_calls;
struct rte_ring init_ring;

static int a(void *arg) {
    void *o;
    rte_ring_enqueue(elem, 0);
    rte_ring_enqueue(&init_ring, 0);
    rte_ring_enqueue(local_flags, 0);
    rte_ring_enqueue(sc_calls, 0);
    rte_ring_mp_enqueue(mp_calls, 0);
    return 0;
}

static int b(void *arg) {
    void *o;
    rte_ring_dequeue(elem, &o);
    rte_ring_dequeue(&init_ring, &o);
    rte_ring_dequeue(local_flags, &o);
    rte_ring_sc_dequeue(sc_calls, &o);
    rte_ring_mp_enqueue(mp_calls, 0);
    return 0;
}

static int c(void *arg) {
    void *o;
    rte_ring_dequeue(&init_ring, &o);
    rte_ring_sc_dequeue(sc_calls, &o);
    rte_ring_dequeue(mp_calls, &o);
    return 0;
}

int main(void) {
    unsigned f = RING_F_SP_ENQ | RING_F_SC_DEQ;
    elem = rte_ring_create_elem("e", 16, 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    rte_ring_init(&init_ring, "i", 64, RING_F_SC_DEQ);
    local_flags = rte_ring_create("l", 1024, SOCKET_ID_ANY, f);
    sc_calls = rte_ring_create("s", 1024, SOCKET_ID_ANY, 0);
    mp_calls = rte_ring_create("m", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    rte_eal_remote_launch(a, 0, 1);
    rte_eal_remote_launch(b, 0, 2);
    rte_eal_remote_launch(c, 0, 3);
    rte_eal_mp_wait_lcore();
    return 0;
}
