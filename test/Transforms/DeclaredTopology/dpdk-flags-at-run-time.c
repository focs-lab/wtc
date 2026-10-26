/* Flags computed from the program's configuration, as DPDK's bbdev_app
   derives them from its core masks: what is declared is not known before
   the run, and the check says so instead of guessing. That holds for a
   side the flags may or may not cover even when the other side is
   declared by a call: `partial` is single-producer by rte_ring_sp_enqueue,
   and single-consumer only if the run gives it RING_F_SC_DEQ, with two
   lcores dequeuing; it is neither verified nor passed over.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s

   CHECK-DAG: warning: wtc: queue *@ring cannot be checked: its flags are computed at run time
   CHECK-DAG: warning: wtc: queue *@partial cannot be checked: its flags are computed at run time
   CHECK-NOT: verified
   CHECK-NOT: error
*/

#include "rte_ring.h"

struct rte_ring *ring, *partial;

static int enc(void *arg) { rte_ring_enqueue(ring, 0); return 0; }
static int dec(void *arg) { void *o; rte_ring_dequeue(ring, &o); return 0; }
static int put(void *arg) { rte_ring_sp_enqueue(partial, 0); return 0; }
static int get(void *arg) { void *o; rte_ring_dequeue(partial, &o); return 0; }

int main(int argc, char **argv) {
    unsigned flags = (argc > 1 ? RING_F_SP_ENQ : 0) | (argc > 2 ? RING_F_SC_DEQ : 0);
    ring = rte_ring_create("enc_to_dec", 1024, SOCKET_ID_ANY, flags);
    partial = rte_ring_create("partial", 1024, SOCKET_ID_ANY, argc > 1 ? RING_F_SC_DEQ : 0);
    for (unsigned i = 1; i < (unsigned)argc; i++) rte_eal_remote_launch(enc, 0, i);
    rte_eal_remote_launch(dec, 0, 99);
    rte_eal_remote_launch(put, 0, 100);
    rte_eal_remote_launch(get, 0, 101);
    rte_eal_remote_launch(get, 0, 102);
    rte_eal_mp_wait_lcore();
    return 0;
}
