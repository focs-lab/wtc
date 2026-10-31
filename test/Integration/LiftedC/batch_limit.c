/* A batch's limit is the program's, every value of its type: a size_t of
   -1 is no limit (the batch moves all it can), an int below zero none.
   The runtime used to take it as an int, and a size_t of -1 became -1,
   no element moved. A limit converted on its way to the comparison into a
   type that does not hold all of its values is refused. The queue is
   dedup's (dedup.c) without its assertions.

   RUN: %wtc_cc -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
   RUN: %t.exe | FileCheck %s --check-prefix=WIDE
   RUN: %wtc_cc -O1 --no-lift %s -o %t.orig.exe
   RUN: %t.orig.exe | FileCheck %s --check-prefix=WIDE
   RUN: %wtc_cc -O1 -DWTC_CHECK -DSIGNED %s -o %t.signed.exe 2>&1 | FileCheck %s
   RUN: %t.signed.exe | FileCheck %s --check-prefix=SIGNED
   RUN: %wtc_cc -O1 --no-lift -DSIGNED %s -o %t.signed.orig.exe
   RUN: %t.signed.orig.exe | FileCheck %s --check-prefix=SIGNED
   RUN: %clang -fclangir -emit-cir -DNARROW %s -o %t.narrow.cir
   RUN: wtc-opt %t.narrow.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=NARROW

   CHECK: C queue struct bq lifted (bq_init: init, bq_destroy: destroy, bq_sender_done: terminate, bq_get_many: batched pop, bq_put_many: batched push)
   WIDE: put 3 got 3
   SIGNED: put 0 got -1
   NARROW: C queue struct bq not lifted: (c) bq_put_many converts its limit, parameter 2, to a type that does not hold all of its values
*/
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(SIGNED)
typedef int limit_t;
#define NO_LIMIT (-1)
#else
typedef size_t limit_t;
#define NO_LIMIT ((size_t)-1)
#endif

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
  for (i = 0; i < max && !fifo_empty(&q->ring) && !fifo_full(into); i++) fifo_put(into, fifo_get(&q->ring));
  if (i > 0) pthread_cond_signal(&q->can_put);
  pthread_mutex_unlock(&q->mu);
  return i;
}

int bq_put_many(struct bq *q, struct fifo *from, limit_t max) {
  int i;
  pthread_mutex_lock(&q->mu);
  while (fifo_full(&q->ring)) pthread_cond_wait(&q->can_put, &q->mu);
#if defined(NARROW)
  for (i = 0; i < (int)max && !fifo_full(&q->ring) && !fifo_empty(from); i++)
#else
  for (i = 0; i < max && !fifo_full(&q->ring) && !fifo_empty(from); i++)
#endif
    fifo_put(&q->ring, fifo_get(from));
  if (i > 0) pthread_cond_signal(&q->can_get);
  pthread_mutex_unlock(&q->mu);
  return i;
}

struct bq *q;
int main(void) {
  q = (struct bq *)malloc(sizeof(struct bq));
  bq_init(q, 8, 1);
  struct fifo out, in;
  fifo_init(&out, 4);
  fifo_init(&in, 4);
  for (long i = 1; i <= 3; ++i) fifo_put(&out, (void *)i);
  int a = bq_put_many(q, &out, NO_LIMIT);
  bq_sender_done(q);
  int b = bq_get_many(q, &in, NO_LIMIT);
  printf("put %d got %d\n", a, b);
  bq_destroy(q);
  free(q);
  return 0;
}
