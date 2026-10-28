/* A pool's declaration as written, and where it cannot be read. The flags
   or the ops are reported as the program writes them, also on a side where
   no access is seen. Flags computed at run time, ops named at run time
   (bbdev_app's style, and testpmd's --mbuf-pool-ops-name), or two
   settings of different ops leave the declaration unknown, and the check
   says so instead of guessing (RUNTIME). A pool found by name may be used
   by another process, and the verdict says so (LOOKUP); one the analysis
   cannot place -- what rte_mempool_lookup hands back here -- makes every
   pool unknown (UNPLACED).

   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
   RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DRUNTIME -o %t.runtime.cir
   RUN: wtc-opt %t.runtime.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=RUNTIME
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DLOOKUP -o %t.lookup.cir
   RUN: not wtc-opt %t.lookup.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOOKUP
   RUN: %clang -std=gnu11 -I%S/Inputs -fclangir -emit-cir %s -DUNPLACED -o %t.unplaced.cir
   RUN: wtc-opt %t.unplaced.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=UNPLACED

   CHECK-DAG: remark: wtc: queue *@one_getter: declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET; no producer access seen), verified in this translation unit
   CHECK-DAG: error: wtc: queue *@two_getters is declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET; no producer access seen) but 2 threads pop from it
   CHECK-DAG: remark: wtc: queue *@no_getter: declared mpsc (by its ops "ring_mp_sc"; no consumer access seen), verified in this translation unit
   CHECK-NOT: declared mpmc

   RUNTIME-DAG: warning: wtc: queue *@by_flags cannot be checked: its flags are computed at run time
   RUNTIME-DAG: warning: wtc: queue *@by_name cannot be checked: its ops are chosen at run time
   RUNTIME-DAG: warning: wtc: queue *@twice cannot be checked: it is given different ops
   RUNTIME-NOT: verified
   RUNTIME-NOT: error

   LOOKUP-DAG: remark: wtc: queue *@one_getter: declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET; no producer access seen), verified in this translation unit; rte_mempool_lookup hands pools to code that finds them by name
   LOOKUP-DAG: error: wtc: queue *@two_getters is declared spsc

   UNPLACED-DAG: remark: wtc: queue access is unresolvable (the queue object could not be identified); every queue of this type is now unknown
   UNPLACED-DAG: warning: wtc: queue *@one_getter is declared spsc (by its flags RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET; no producer access seen); cannot verify: the queue object could not be identified
   UNPLACED-NOT: verified
   UNPLACED-NOT: error
*/

#include "rte_mempool.h"

struct rte_mempool *one_getter, *two_getters, *no_getter, *by_flags, *by_name, *twice;

static int a(void *arg) {
    void *o;
#if defined(RUNTIME)
    rte_mempool_put(by_flags, 0);
    rte_mempool_put(by_name, 0);
    rte_mempool_put(twice, 0);
#elif defined(UNPLACED)
    rte_mempool_get(one_getter, &o);
    rte_mempool_put(rte_mempool_lookup("peer"), 0);
#else
    rte_mempool_get(one_getter, &o);
    rte_mempool_get(two_getters, &o);
    rte_mempool_put(no_getter, 0);
#endif
    return 0;
}

static int b(void *arg) {
    void *o;
#if defined(RUNTIME)
    rte_mempool_put(by_flags, 0);
    rte_mempool_put(by_name, 0);
    rte_mempool_put(twice, 0);
#else
    rte_mempool_get(two_getters, &o);
#endif
    return 0;
}

int main(int argc, char **argv) {
#if defined(RUNTIME)
    by_flags = rte_mempool_create("f", 1023, 64, 0, 0, 0, 0, 0, 0, SOCKET_ID_ANY, argc > 1 ? RTE_MEMPOOL_F_SP_PUT : 0);
    by_name = rte_pktmbuf_pool_create_by_ops("n", 1023, 32, 0, 2048, SOCKET_ID_ANY, argv[1]);
    twice = rte_mempool_create_empty("t", 1023, 64, 32, 0, SOCKET_ID_ANY, 0);
    rte_mempool_set_ops_byname(twice, "ring_sp_sc", 0);
    rte_mempool_set_ops_byname(twice, "ring_mp_mc", 0);
#else
    one_getter = rte_mempool_create("o", 1023, 64, 0, 0, 0, 0, 0, 0, SOCKET_ID_ANY,
                                    RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    two_getters = rte_mempool_create("t", 1023, 64, 0, 0, 0, 0, 0, 0, SOCKET_ID_ANY,
                                     RTE_MEMPOOL_F_SP_PUT | RTE_MEMPOOL_F_SC_GET);
    no_getter = rte_pktmbuf_pool_create_by_ops("n", 1023, 32, 0, 2048, SOCKET_ID_ANY, "ring_mp_sc");
#endif
#if defined(LOOKUP)
    struct rte_mempool *peer = rte_mempool_lookup("peer");
    (void)peer;
#endif
    rte_eal_remote_launch(a, 0, 1);
    rte_eal_remote_launch(b, 0, 2);
    rte_eal_mp_wait_lcore();
    return 0;
}
