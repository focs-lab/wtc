/* Rings the walk cannot tell apart, one per lcore in an array of
   pointers; rings initialised by rte_ring_init on the elements of an
   array, which are counted together; and enqueues made through a pointer
   to rte_ring_sp_enqueue, set in a function or by a file-scope
   initialiser, which no call site shows. None is verified: the first and
   the last make every ring unknown, and the array of rings is a summary
   whose union cannot be held against one element's declaration.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DARRAY -o %t.array.cir
   RUN: wtc-opt %t.array.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=ARRAY
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.pointer.cir
   RUN: wtc-opt %t.pointer.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=POINTER
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DFILE_SCOPE -o %t.file.cir
   RUN: wtc-opt %t.file.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=POINTER
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DINIT_ARRAY -o %t.init.cir
   RUN: wtc-opt %t.init.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=INIT

   ARRAY: remark: wtc: queue access is unresolvable ({{.*}}); every queue of this type is now unknown
   ARRAY-NOT: verified
   POINTER: remark: wtc: the address of rte_ring_sp_enqueue is taken, and calls through it are not counted; every queue of its type is now unknown
   POINTER: warning: wtc: queue *@ring is declared mpsc (by its flags); cannot verify: the address of rte_ring_sp_enqueue is taken, and calls through it are not counted
   POINTER-NOT: verified
   INIT: remark: wtc: queue @storage[*]: mpsc (producers: many, consumers: 1)
   INIT: warning: wtc: queue @storage[*] is declared spsc (by its flags); cannot verify: it stands for every element of an array, counted together, and each element may have threads of its own
   INIT-NOT: verified
   INIT-NOT: error
*/

#include "rte_ring.h"

#ifdef ARRAY
struct rte_ring *rings[4];

static int worker(void *arg) {
    unsigned id = (unsigned)(unsigned long)arg;
    void *o;
    rte_ring_enqueue(rings[id], 0);
    rte_ring_dequeue(rings[(id + 1) % 4], &o);
    return 0;
}

int main(void) {
    for (unsigned i = 0; i < 4; i++) rings[i] = rte_ring_create("r", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    for (unsigned i = 0; i < 4; i++) rte_eal_remote_launch(worker, (void *)(unsigned long)i, i + 1);
    rte_eal_mp_wait_lcore();
    return 0;
}
#elif defined(INIT_ARRAY)
struct rte_ring storage[2];

static int producer(void *arg) { rte_ring_enqueue(&storage[0], 0); return 0; }
static int consumer(void *arg) { void *o; rte_ring_dequeue(&storage[0], &o); return 0; }

int main(void) {
    for (unsigned i = 0; i < 2; i++) rte_ring_init(&storage[i], "r", 64, RING_F_SP_ENQ | RING_F_SC_DEQ);
    rte_eal_remote_launch(producer, 0, 1);
    rte_eal_remote_launch(producer, 0, 2);
    rte_eal_remote_launch(consumer, 0, 3);
    rte_eal_mp_wait_lcore();
    return 0;
}
#else
struct rte_ring *ring;
#ifdef FILE_SCOPE
int (*enq)(struct rte_ring *, void *) = rte_ring_sp_enqueue;
#else
int (*enq)(struct rte_ring *, void *);
#endif

static int producer(void *arg) { enq(ring, 0); return 0; }
static int consumer(void *arg) { void *o; rte_ring_dequeue(ring, &o); return 0; }

int main(void) {
    ring = rte_ring_create("r", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
#ifndef FILE_SCOPE
    enq = rte_ring_sp_enqueue;
#endif
    rte_eal_remote_launch(producer, 0, 1);
    rte_eal_remote_launch(producer, 0, 2);
    rte_eal_remote_launch(consumer, 0, 3);
    rte_eal_mp_wait_lcore();
    return 0;
}
#endif
