/* The shape of DPDK's ring and launch API that the analysis models, with
 * the names, operand order and flag values of DPDK 26.11 (rte_ring.h,
 * rte_ring_core.h, rte_launch.h). The bodies stand in for DPDK's: the
 * default enqueue and dequeue dispatch on the ring's flags to the single-
 * and multi-thread forms, as DPDK's do, so a test shows that those inner
 * calls are the library's and declare nothing. */
#ifndef WTC_TEST_RTE_RING_H
#define WTC_TEST_RTE_RING_H

#define RING_F_SP_ENQ 0x0001
#define RING_F_SC_DEQ 0x0002
#define SOCKET_ID_ANY -1

struct rte_ring {
    int sp, sc;
    void *slots[64];
    unsigned head, tail;
};

struct rte_ring *rte_ring_create(const char *name, unsigned count, int socket_id, unsigned flags);
struct rte_ring *rte_ring_create_elem(const char *name, unsigned esize, unsigned count, int socket_id,
                                      unsigned flags);
int rte_ring_init(struct rte_ring *r, const char *name, unsigned count, unsigned flags);
struct rte_ring *rte_ring_lookup(const char *name);

static inline int rte_ring_sp_enqueue_bulk_elem(struct rte_ring *r, void *obj) {
    r->slots[r->head++ % 64] = obj;
    return 0;
}
static inline int rte_ring_mp_enqueue_bulk_elem(struct rte_ring *r, void *obj) {
    r->slots[__atomic_fetch_add(&r->head, 1, __ATOMIC_ACQ_REL) % 64] = obj;
    return 0;
}
static inline int rte_ring_sc_dequeue_bulk_elem(struct rte_ring *r, void **obj) {
    *obj = r->slots[r->tail++ % 64];
    return 0;
}
static inline int rte_ring_mc_dequeue_bulk_elem(struct rte_ring *r, void **obj) {
    *obj = r->slots[__atomic_fetch_add(&r->tail, 1, __ATOMIC_ACQ_REL) % 64];
    return 0;
}

static inline int rte_ring_enqueue(struct rte_ring *r, void *obj) {
    return r->sp ? rte_ring_sp_enqueue_bulk_elem(r, obj) : rte_ring_mp_enqueue_bulk_elem(r, obj);
}
static inline int rte_ring_sp_enqueue(struct rte_ring *r, void *obj) { return rte_ring_sp_enqueue_bulk_elem(r, obj); }
static inline int rte_ring_mp_enqueue(struct rte_ring *r, void *obj) { return rte_ring_mp_enqueue_bulk_elem(r, obj); }
static inline int rte_ring_dequeue(struct rte_ring *r, void **obj) {
    return r->sc ? rte_ring_sc_dequeue_bulk_elem(r, obj) : rte_ring_mc_dequeue_bulk_elem(r, obj);
}
static inline int rte_ring_sc_dequeue(struct rte_ring *r, void **obj) { return rte_ring_sc_dequeue_bulk_elem(r, obj); }
static inline int rte_ring_mc_dequeue(struct rte_ring *r, void **obj) { return rte_ring_mc_dequeue_bulk_elem(r, obj); }

typedef int(lcore_function_t)(void *);
enum rte_rmt_call_main_t { SKIP_MAIN = 0, CALL_MAIN };
int rte_eal_remote_launch(lcore_function_t *f, void *arg, unsigned worker_id);
int rte_eal_mp_remote_launch(lcore_function_t *f, void *arg, enum rte_rmt_call_main_t call_main);
int rte_eal_mp_wait_lcore(void);

/* A DPDK function that takes rings and has no body here
 * (drivers/net/ring/rte_eth_ring.h). */
int rte_eth_from_rings(const char *name, struct rte_ring *const rx_queues[], unsigned nb_rx_queues,
                       struct rte_ring *const tx_queues[], unsigned nb_tx_queues, unsigned numa_node);

#endif
