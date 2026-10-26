/* A ring is named shared memory: another process can find it by name, a
   DPDK function with no body here can take it, whole, cast to a void *
   (a callback's context), or inside a structure of parameters or a
   per-lcore configuration holding an array of rings, handed over by its
   type or as a void *, as ip_pipeline hands its software queues to the
   port library. A verdict on a ring is therefore about this
   translation unit only, and says so; it names the reason when it sees
   one, and a ring flagged for many threads is not called more cautious
   than needed when code out of sight may be among them.

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DLOOKUP -o %t.lookup.cir
   RUN: wtc-opt %t.lookup.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOOKUP
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DHANDED -o %t.handed.cir
   RUN: wtc-opt %t.handed.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=HANDED
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DPARAMS -o %t.params.cir
   RUN: wtc-opt %t.params.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=PARAMS
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DTYPED -o %t.typed.cir
   RUN: wtc-opt %t.typed.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=TYPED
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DVOIDCAST -o %t.voidcast.cir
   RUN: wtc-opt %t.voidcast.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=VOIDCAST
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DCONF_TYPED -o %t.conftyped.cir
   RUN: wtc-opt %t.conftyped.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CONF_TYPED
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DCONF_VOID -o %t.confvoid.cir
   RUN: wtc-opt %t.confvoid.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CONF_VOID

   LOOKUP: remark: wtc: queue *@ring: declared spsc (by its flags), verified in this translation unit; rte_ring_lookup hands rings to code that finds them by name
   LOOKUP-NOT: is declared mpmc
   HANDED: remark: wtc: queue *@ring: declared spsc (by its flags), verified in this translation unit; it is handed to rte_eth_from_rings, whose body is not here
   HANDED-NOT: is declared mpmc
   PARAMS: remark: wtc: queue *@ring: declared spsc (by its flags), verified in this translation unit; it is handed to rte_port_ring_writer_create, whose body is not here
   PARAMS-NOT: is declared mpmc
   TYPED: remark: wtc: queue *@ring: declared spsc (by its flags), verified in this translation unit; it is handed to port_writer_from_params, whose body is not here
   TYPED-NOT: is declared mpmc
   VOIDCAST: remark: wtc: queue *@ring: declared spsc (by its flags), verified in this translation unit; it is handed to set_context, whose body is not here
   CONF_TYPED: remark: wtc: queue *@ring: declared spsc (by its flags), verified in this translation unit; it is handed to apply_conf, whose body is not here
   CONF_TYPED-NOT: is declared mpmc
   CONF_VOID: remark: wtc: queue *@ring: declared spsc (by its flags), verified in this translation unit; it is kept in a structure, and set_context, whose body is not here, takes a void * that may hold one
   CONF_VOID-NOT: is declared mpmc
*/

#include "rte_ring.h"

struct rte_ring *ring, *plain;

/* rte_port_ring_writer_create(void *params, int socket_id), from DPDK's
   port library, which keeps the ring of its rte_port_ring_writer_params. */
struct rte_port_ring_writer_params {
    struct rte_ring *ring;
    unsigned tx_burst_sz;
};
void *rte_port_ring_writer_create(void *params, int socket_id);
/* A function that takes the structure by its own type, which a reader of
   the call sees as plainly as a ring handed over. */
void *port_writer_from_params(struct rte_port_ring_writer_params *params);
/* A callback's context, kept by code out of sight. */
void set_context(void *ctx);
/* DPDK's usual per-lcore configuration: an array of rings. */
struct lcore_conf {
    struct rte_ring *rings[4];
    unsigned n_rings;
};
void apply_conf(struct lcore_conf *conf);
struct lcore_conf conf;
void *ctx;

static int producer(void *arg) {
    rte_ring_enqueue(ring, 0);
    rte_ring_enqueue(plain, 0);
    return 0;
}

static int consumer(void *arg) {
    void *o;
    rte_ring_dequeue(ring, &o);
    rte_ring_dequeue(plain, &o);
    return 0;
}

int main(void) {
    ring = rte_ring_create("r", 1024, SOCKET_ID_ANY, RING_F_SP_ENQ | RING_F_SC_DEQ);
    plain = rte_ring_create("p", 1024, SOCKET_ID_ANY, 0);
#if defined(LOOKUP)
    struct rte_ring *peer = rte_ring_lookup("peer");
    (void)peer;
#elif defined(HANDED)
    struct rte_ring *queues[1] = {plain};
    rte_eth_from_rings("net_ring0", queues, 1, queues, 1, 0);
#elif defined(PARAMS)
    struct rte_port_ring_writer_params params = {plain, 32};
    rte_port_ring_writer_create(&params, 0);
#elif defined(VOIDCAST)
    set_context((void *)plain);
#elif defined(CONF_TYPED)
    for (unsigned i = 0; i < 4; i++) conf.rings[i] = plain;
    apply_conf(&conf);
#elif defined(CONF_VOID)
    for (unsigned i = 0; i < 4; i++) conf.rings[i] = plain;
    ctx = &conf;
    set_context(ctx);
#else
    struct rte_port_ring_writer_params params = {plain, 32};
    port_writer_from_params(&params);
#endif
    rte_eal_remote_launch(producer, 0, 1);
    rte_eal_remote_launch(consumer, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
