/* The batches of a queue (dedup's, as LiftedC/batch_limit.c writes it)
   move elements between the queue's ring and a ring of the calling
   thread's own: a local of the caller that only direct calls reach.
   Refused: a batch handed a global ring (GLOBAL_RING), or a local whose
   address the caller stores (LEAK_RING), either of which another thread may
   reach; a loop that moves elements from the queue's ring into itself
   (WITHIN); and a function of the queue that moves elements between two
   rings neither of which is the queue's (OUTSIDE), which until 28 Nov 2026
   was said to move them "within the queue".

   RUN: %clang -fclangir -emit-cir -O1 %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=LIFTS
   RUN: %clang -fclangir -emit-cir -O1 -DGLOBAL_RING %s -o %t.global.cir
   RUN: wtc-opt %t.global.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=GLOBAL
   RUN: %clang -fclangir -emit-cir -O1 -DLEAK_RING %s -o %t.leak.cir
   RUN: wtc-opt %t.leak.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=LEAK
   RUN: %clang -fclangir -emit-cir -O1 -DWITHIN %s -o %t.within.cir
   RUN: wtc-opt %t.within.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=WITHIN
   RUN: %clang -fclangir -emit-cir -O1 -DOUTSIDE %s -o %t.outside.cir
   RUN: wtc-opt %t.outside.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=OUTSIDE

   LIFTS: C queue struct bq lifts (bq_init: init, bq_destroy: destroy, bq_sender_done: terminate, bq_get_many: batched pop, bq_put_many: batched push)
   GLOBAL: C queue struct bq not lifted: (c) a call of bq_get_many passes a ring that is not the caller's local
   LEAK: C queue struct bq not lifted: (c) the ring a call of bq_get_many passes may be reached by another thread (cir.store)
   WITHIN: C queue struct bq not lifted: (a) bq_get_many has a loop that moves elements within one ring
   OUTSIDE: C queue struct bq not lifted: (e) bq_shuffle moves elements between two rings, neither of them the queue's
*/
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef size_t limit_t;
#define NO_LIMIT ((size_t)-1)

struct fifo {
  int wr, rd;
  void **cell;
  size_t n;
};

static inline int fifo_init(struct fifo *f, size_t cap) {
  f->cell = (void **)malloc(sizeof(void *) * (cap + 1));
  f->n = cap + 1;
  f->wr = f->rd = 0;
  return f->cell == NULL;
}
static inline void fifo_free(struct fifo *f) { free(f->cell); }
static inline int fifo_empty(struct fifo *f) { return f->rd == f->wr; }
static inline int fifo_full(struct fifo *f) { return f->wr == (f->rd - 1 + f->n) % f->n; }
static inline void *fifo_get(struct fifo *f) {
  void *p = NULL;
  if (!fifo_empty(f)) {
    p = f->cell[f->rd];
    f->rd++;
    if (f->rd >= f->n) f->rd = 0;
  }
  return p;
}
static inline int fifo_put(struct fifo *f, void *p) {
  if (fifo_full(f)) return -1;
  f->cell[f->wr] = p;
  f->wr++;
  if (f->wr == f->n) f->wr = 0;
  return 0;
}

struct bq {
  struct fifo ring;
  int senders, closed;
  pthread_mutex_t mu;
  pthread_cond_t can_get, can_put;
};

void bq_init(struct bq *q, size_t cap, int senders) {
  pthread_mutex_init(&q->mu, NULL);
  pthread_cond_init(&q->can_get, NULL);
  pthread_cond_init(&q->can_put, NULL);
  fifo_init(&q->ring, cap);
  q->senders = senders;
  q->closed = 0;
}

void bq_destroy(struct bq *q) {
  pthread_cond_destroy(&q->can_put);
  pthread_cond_destroy(&q->can_get);
  pthread_mutex_destroy(&q->mu);
  fifo_free(&q->ring);
}

static inline int bq_finished(struct bq *q) { return q->closed == q->senders; }

void bq_sender_done(struct bq *q) {
  pthread_mutex_lock(&q->mu);
  q->closed++;
  if (bq_finished(q)) pthread_cond_broadcast(&q->can_get);
  pthread_mutex_unlock(&q->mu);
}

int bq_get_many(struct bq *q, struct fifo *into, limit_t max) {
  int i;
  pthread_mutex_lock(&q->mu);
  while (fifo_empty(&q->ring) && !bq_finished(q)) pthread_cond_wait(&q->can_get, &q->mu);
  if (fifo_empty(&q->ring) && bq_finished(q)) {
    pthread_mutex_unlock(&q->mu);
    return -1;
  }
#if defined(WITHIN)
  for (i = 0; i < max && !fifo_empty(&q->ring) && !fifo_full(&q->ring); i++) {
    void *p = q->ring.cell[q->ring.rd];
    q->ring.rd++;
    if (q->ring.rd >= q->ring.n) q->ring.rd = 0;
    q->ring.cell[q->ring.wr] = p;
    q->ring.wr++;
    if (q->ring.wr == q->ring.n) q->ring.wr = 0;
  }
#else
  for (i = 0; i < max && !fifo_empty(&q->ring) && !fifo_full(into); i++) fifo_put(into, fifo_get(&q->ring));
#endif
  if (i > 0) pthread_cond_signal(&q->can_put);
  pthread_mutex_unlock(&q->mu);
  return i;
}

int bq_put_many(struct bq *q, struct fifo *from, limit_t max) {
  int i;
  pthread_mutex_lock(&q->mu);
  while (fifo_full(&q->ring)) pthread_cond_wait(&q->can_put, &q->mu);
  for (i = 0; i < max && !fifo_full(&q->ring) && !fifo_empty(from); i++)
    fifo_put(&q->ring, fifo_get(from));
  if (i > 0) pthread_cond_signal(&q->can_get);
  pthread_mutex_unlock(&q->mu);
  return i;
}

#if defined(OUTSIDE)
int bq_shuffle(struct bq *q, struct fifo *from, struct fifo *into, limit_t max) {
  int i;
  pthread_mutex_lock(&q->mu);
  for (i = 0; i < max && !fifo_empty(from) && !fifo_full(into); i++) fifo_put(into, fifo_get(from));
  pthread_mutex_unlock(&q->mu);
  return i;
}
#endif
struct bq *q;
struct fifo g_in;
struct fifo *g_seen;
int main(void) {
  q = (struct bq *)malloc(sizeof(struct bq));
  bq_init(q, 8, 1);
  struct fifo out, in;
  fifo_init(&out, 4);
  fifo_init(&in, 4);
  for (long i = 1; i <= 3; ++i) fifo_put(&out, (void *)i);
  int a = bq_put_many(q, &out, NO_LIMIT);
  bq_sender_done(q);
#if defined(GLOBAL_RING)
  fifo_init(&g_in, 4);
  int b = bq_get_many(q, &g_in, NO_LIMIT);
#else
  int b = bq_get_many(q, &in, NO_LIMIT);
#endif
#if defined(LEAK_RING)
  g_seen = &in;
#endif
#if defined(OUTSIDE)
  bq_shuffle(q, &in, &out, NO_LIMIT);
#endif
  printf("put %d got %d\n", a, b);
  bq_destroy(q);
  free(q);
  return 0;
}
