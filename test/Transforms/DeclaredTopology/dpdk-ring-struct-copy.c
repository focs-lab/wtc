/* A ring handed to each lcore inside a heap structure written whole from a
   compound literal, as DPDK's distributor sample writes its lcore
   parameters: `*p = (struct lcore_params){...}`. The literal is built field
   by field in a slot of its own and copied into the heap object; the copy
   moves values the per-field listing already holds, so the ring each lcore
   loads from `p->ring` is placed, and the declaration is checked. The
   blocks go back to the EAL heap with rte_free, whose body is not here: it
   writes no field. With WRAP, rte_free is defined in the module and writes
   bytes of the block it is handed: its body is read, and the record is
   given up. So with MALLOC, an allocator by name, rte_malloc, defined here
   and writing through what it is handed; and with MEMALIGN, a record
   handed to posix_memalign as its out-parameter, which it writes.
   With a copy out of a view of another type (VIEW), or with memcpy, which
   may copy part of a record (MEMCPY), the record is given up as before,
   and nothing is verified.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DVIEW -o %t.view.cir
   RUN: wtc-opt %t.view.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=VIEW
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DMEMCPY -o %t.memcpy.cir
   RUN: wtc-opt %t.memcpy.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=VIEW
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DWRAP -o %t.wrap.cir
   RUN: wtc-opt %t.wrap.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=VIEW
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DMALLOC -o %t.malloc.cir
   RUN: wtc-opt %t.malloc.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=VIEW
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DMEMALIGN -o %t.memalign.cir
   RUN: wtc-opt %t.memalign.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=VIEW

   CHECK: queue {{.*}}: spsc (producers: 1, consumers: 1)
   CHECK: declared spsc (by its flags RING_F_SP_ENQ | RING_F_SC_DEQ), verified in this translation unit
   CHECK-NOT: unresolvable
   VIEW-NOT: verified
*/

#include <stdlib.h>
#include <string.h>
#include "rte_ring.h"

struct lcore_params {
    unsigned id;
    struct rte_ring *ring;
};

#ifdef MALLOC
void *rte_malloc(const char *type, size_t size, unsigned align) {
    unsigned char *bytes = (unsigned char *)type;
    bytes[sizeof(void *)] = 0x55;   /* a byte of the ring pointer */
    (void)align;
    return malloc(size);
}
#endif

#ifdef WRAP
void rte_free(void *ptr) {
    unsigned char *bytes = ptr;
    bytes[sizeof(void *)] = 0x55;   /* a byte of the ring pointer */
}
#endif

static int producer(void *arg) {
    struct lcore_params *p = arg;
    rte_ring_enqueue(p->ring, 0);
    return 0;
}

static int consumer(void *arg) {
    struct lcore_params *p = arg;
    void *o;
    rte_ring_dequeue(p->ring, &o);
    return 0;
}

int main(void) {
    struct rte_ring *r = rte_ring_create("r", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    struct lcore_params *a = malloc(sizeof *a), *b = malloc(sizeof *b);
#if defined(VIEW)
    unsigned char raw[sizeof *a];
    memset(raw, 0, sizeof raw);
    *a = *(struct lcore_params *)raw;
#elif defined(MEMCPY)
    struct lcore_params tmp = {1, r};
    memcpy(a, &tmp, sizeof tmp.id);
#else
    *a = (struct lcore_params){1, r};
#endif
    *b = (struct lcore_params){2, r};
#if defined(MALLOC)
    void *spare = rte_malloc((const char *)a, 64, 0);
    (void)spare;
#elif defined(MEMALIGN)
    posix_memalign((void **)a, 64, 64);
#endif
    rte_eal_remote_launch(producer, a, 1);
    rte_eal_remote_launch(consumer, b, 2);
    rte_eal_mp_wait_lcore();
    rte_free(a);
    rte_free(b);
    return 0;
}
