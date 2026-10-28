/* The shape of DPDK's mempool and mbuf API that the analysis models, with
 * the names, operand order and flag values of DPDK 26.11 (rte_mempool.h,
 * rte_mbuf.h, rte_mbuf_core.h). The bodies stand in for DPDK's: a put or a
 * get goes to the calling lcore's cache when the pool has one, and to the
 * pool's ops -- a call through a pointer -- when the cache is full, empty
 * or absent; a free puts the mbuf into the pool its `pool` field names. A
 * test thus shows that those inner calls are the library's and declare
 * nothing. */
#ifndef WTC_TEST_RTE_MEMPOOL_H
#define WTC_TEST_RTE_MEMPOOL_H

#include "rte_ring.h"

#define RTE_MEMPOOL_F_NO_CACHE_ALIGN 0x0002
#define RTE_MEMPOOL_F_SP_PUT 0x0004
#define RTE_MEMPOOL_F_SC_GET 0x0008
/* The names before DPDK 21.11, still defined. */
#define MEMPOOL_F_NO_CACHE_ALIGN RTE_MEMPOOL_F_NO_CACHE_ALIGN
#define MEMPOOL_F_SP_PUT RTE_MEMPOOL_F_SP_PUT
#define MEMPOOL_F_SC_GET RTE_MEMPOOL_F_SC_GET
#define RTE_MEMPOOL_CACHE_MAX_SIZE 512
#define LCORE_ID_ANY 0xffffffffu
#define NULL ((void *)0)

struct rte_mempool;
struct rte_mempool_ops {
    int (*enqueue)(struct rte_mempool *mp, void *const *obj_table, unsigned n);
    int (*dequeue)(struct rte_mempool *mp, void **obj_table, unsigned n);
};
extern struct rte_mempool_ops rte_mempool_ops_table[16];

struct rte_mempool_cache {
    unsigned size, len;
    void *objs[RTE_MEMPOOL_CACHE_MAX_SIZE * 2];
};

struct rte_mempool {
    char name[32];
    unsigned flags, cache_size;
    int ops_index;
    struct rte_mempool_cache *local_cache;
};

struct rte_mbuf {
    void *buf_addr;
    struct rte_mbuf *next;
    struct rte_mempool *pool;
    unsigned short refcnt, nb_segs;
};

typedef void(rte_mempool_ctor_t)(struct rte_mempool *, void *);
typedef void(rte_mempool_obj_cb_t)(struct rte_mempool *mp, void *opaque, void *obj, unsigned obj_idx);

struct rte_mempool *rte_mempool_create(const char *name, unsigned n, unsigned elt_size, unsigned cache_size,
                                       unsigned private_data_size, rte_mempool_ctor_t *mp_init, void *mp_init_arg,
                                       rte_mempool_obj_cb_t *obj_init, void *obj_init_arg, int socket_id,
                                       unsigned flags);
struct rte_mempool *rte_mempool_create_empty(const char *name, unsigned n, unsigned elt_size, unsigned cache_size,
                                             unsigned private_data_size, int socket_id, unsigned flags);
int rte_mempool_set_ops_byname(struct rte_mempool *mp, const char *name, void *pool_config);
int rte_mempool_populate_default(struct rte_mempool *mp);
struct rte_mempool *rte_mempool_lookup(const char *name);
void rte_mempool_free(struct rte_mempool *mp);
struct rte_mempool_cache *rte_mempool_cache_create(unsigned size, int socket_id);
unsigned rte_lcore_id(void);

struct rte_mempool *rte_pktmbuf_pool_create(const char *name, unsigned n, unsigned cache_size,
                                            unsigned short priv_size, unsigned short data_room_size, int socket_id);
struct rte_mempool *rte_pktmbuf_pool_create_by_ops(const char *name, unsigned n, unsigned cache_size,
                                                   unsigned short priv_size, unsigned short data_room_size,
                                                   int socket_id, const char *ops_name);
void rte_pktmbuf_pool_init(struct rte_mempool *mp, void *opaque_arg);
void rte_pktmbuf_init(struct rte_mempool *mp, void *opaque_arg, void *m, unsigned i);

static inline int rte_mempool_ops_enqueue_bulk(struct rte_mempool *mp, void *const *obj_table, unsigned n) {
    return rte_mempool_ops_table[mp->ops_index].enqueue(mp, obj_table, n);
}
static inline int rte_mempool_ops_dequeue_bulk(struct rte_mempool *mp, void **obj_table, unsigned n) {
    return rte_mempool_ops_table[mp->ops_index].dequeue(mp, obj_table, n);
}
static inline struct rte_mempool_cache *rte_mempool_default_cache(struct rte_mempool *mp, unsigned lcore_id) {
    if (mp->cache_size == 0 || lcore_id == LCORE_ID_ANY) return NULL;
    return &mp->local_cache[lcore_id];
}
static inline void rte_mempool_generic_put(struct rte_mempool *mp, void *const *obj_table, unsigned n,
                                           struct rte_mempool_cache *cache) {
    if (cache == NULL || cache->len + n > cache->size * 2) {
        rte_mempool_ops_enqueue_bulk(mp, obj_table, n);
        return;
    }
    for (unsigned i = 0; i < n; i++) cache->objs[cache->len++] = obj_table[i];
    if (cache->len >= cache->size) {
        rte_mempool_ops_enqueue_bulk(mp, &cache->objs[cache->size], cache->len - cache->size);
        cache->len = cache->size;
    }
}
static inline void rte_mempool_put_bulk(struct rte_mempool *mp, void *const *obj_table, unsigned n) {
    rte_mempool_generic_put(mp, obj_table, n, rte_mempool_default_cache(mp, rte_lcore_id()));
}
static inline void rte_mempool_put(struct rte_mempool *mp, void *obj) { rte_mempool_put_bulk(mp, &obj, 1); }
static inline int rte_mempool_generic_get(struct rte_mempool *mp, void **obj_table, unsigned n,
                                          struct rte_mempool_cache *cache) {
    if (cache == NULL || n > cache->size) return rte_mempool_ops_dequeue_bulk(mp, obj_table, n);
    if (cache->len < n) {
        int ret = rte_mempool_ops_dequeue_bulk(mp, &cache->objs[cache->len], cache->size);
        if (ret < 0) return ret;
        cache->len += cache->size;
    }
    for (unsigned i = 0; i < n; i++) obj_table[i] = cache->objs[--cache->len];
    return 0;
}
static inline int rte_mempool_get_bulk(struct rte_mempool *mp, void **obj_table, unsigned n) {
    return rte_mempool_generic_get(mp, obj_table, n, rte_mempool_default_cache(mp, rte_lcore_id()));
}
static inline int rte_mempool_get(struct rte_mempool *mp, void **obj_p) { return rte_mempool_get_bulk(mp, obj_p, 1); }
static inline void rte_mempool_cache_flush(struct rte_mempool_cache *cache, struct rte_mempool *mp) {
    if (cache == NULL) cache = rte_mempool_default_cache(mp, rte_lcore_id());
    if (cache == NULL || cache->len == 0) return;
    rte_mempool_ops_enqueue_bulk(mp, cache->objs, cache->len);
    cache->len = 0;
}

static inline struct rte_mbuf *rte_mbuf_raw_alloc(struct rte_mempool *mp) {
    void *m;
    if (rte_mempool_get(mp, &m) < 0) return NULL;
    return (struct rte_mbuf *)m;
}
static inline struct rte_mbuf *rte_pktmbuf_alloc(struct rte_mempool *mp) {
    struct rte_mbuf *m = rte_mbuf_raw_alloc(mp);
    if (m) m->next = NULL;
    return m;
}
static inline int rte_pktmbuf_alloc_bulk(struct rte_mempool *pool, struct rte_mbuf **mbufs, unsigned count) {
    return rte_mempool_get_bulk(pool, (void **)mbufs, count);
}
static inline void rte_mbuf_raw_free(struct rte_mbuf *m) { rte_mempool_put(m->pool, m); }
static inline void rte_pktmbuf_free_seg(struct rte_mbuf *m) {
    if (--m->refcnt == 0) rte_mbuf_raw_free(m);
}
static inline void rte_pktmbuf_free(struct rte_mbuf *m) {
    while (m != NULL) {
        struct rte_mbuf *next = m->next;
        rte_pktmbuf_free_seg(m);
        m = next;
    }
}
void rte_pktmbuf_free_bulk(struct rte_mbuf **mbufs, unsigned count);
static inline int rte_pktmbuf_chain(struct rte_mbuf *head, struct rte_mbuf *tail) {
    struct rte_mbuf *cur = head;
    while (cur->next != NULL) cur = cur->next;
    cur->next = tail;
    head->nb_segs += tail->nb_segs;
    return 0;
}
struct rte_mbuf *rte_pktmbuf_copy(const struct rte_mbuf *m, struct rte_mempool *mp, unsigned offset,
                                  unsigned length);
int __rte_pktmbuf_linearize(struct rte_mbuf *mbuf);
static inline int rte_pktmbuf_linearize(struct rte_mbuf *mbuf) {
    if (mbuf->nb_segs == 1) return 0;
    return __rte_pktmbuf_linearize(mbuf);
}
static inline int rte_pktmbuf_adj(struct rte_mbuf *m, unsigned short len) {
    m->buf_addr = (char *)m->buf_addr + len;
    return 0;
}
int rte_mempool_obj_iter(struct rte_mempool *mp, rte_mempool_obj_cb_t *obj_cb, void *obj_cb_arg);

/* The zero-copy cache access proposed for DPDK (not in it as of 26.11). */
void **rte_mempool_cache_zc_put_bulk(struct rte_mempool_cache *cache, struct rte_mempool *mp, unsigned n);
void **rte_mempool_cache_zc_get_bulk(struct rte_mempool_cache *cache, struct rte_mempool *mp, unsigned n);

/* The single- and multi-thread forms DPDK deprecated in 17.05. */
void rte_mempool_sp_put(struct rte_mempool *mp, void *obj);
void rte_mempool_mp_put(struct rte_mempool *mp, void *obj);
int rte_mempool_sc_get(struct rte_mempool *mp, void **obj_p);

/* Hands mbufs to a driver, which frees them once sent (rte_ethdev.h: through
 * the port's function table). */
typedef unsigned short (*eth_tx_burst_t)(void *txq, struct rte_mbuf **tx_pkts, unsigned short nb_pkts);
extern eth_tx_burst_t tx_pkt_burst;
static inline unsigned short rte_eth_tx_burst(unsigned short port_id, unsigned short queue_id,
                                              struct rte_mbuf **tx_pkts, unsigned short nb_pkts) {
    return tx_pkt_burst(0, tx_pkts, nb_pkts);
}

#endif
