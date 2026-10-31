/* A model of the queue PARSEC's dedup keeps in C (kernels/dedup/src/
   queue.h, queue.c) and of the way its encoder uses it (encoder.c),
   written for this test: the queue's ring is a member struct, the ring's
   operations are static inline helpers the threads also use on rings of
   their own, elements move in batches between the queue and a ring on the
   calling thread's stack, up to a limit, and the queues are arrays on the
   heap, as dedup's are. Three stages instead of five, a long per chunk.

   A batched pop takes adjacent elements at one step, whatever the number of
   consumers: with one thread a stage the queues lift as spsc, and with two
   consumers on the first queue (TWO) the struct lifts too, the first queue
   spmc and the second mpsc, and runs with WTC_CHECK and under
   ThreadSanitizer.

   dedup's operations assert on the queue's state and on the elements
   (no push after the queue has finished, no more terminations than
   producers, no null element), checks the substitute does not make: with
   them (PARSEC_ASSERTS) the struct is refused, as dedup's own is. Without
   them it lifts; the initialisation's assertion on its allocation stays,
   the one check the substitute makes as well.

   RUN: %wtc_cc -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
   RUN: %t.exe | FileCheck %s --check-prefix=OUT
   RUN: %wtc_cc -O1 -DWTC_CHECK -DTWO %s -o %t.two.exe 2>&1 | FileCheck %s --check-prefix=TWO
   RUN: %t.two.exe | FileCheck %s --check-prefix=OUT
   RUN: %if tsan %{ %wtc_cc --tsan -O1 -g -DTWO %s -o %t.two.tsan.exe 2>&1 | FileCheck %s --check-prefix=TWO %}
   RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.two.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
   RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s %}
   RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
   RUN: %clang -fclangir -emit-cir -DPARSEC_ASSERTS %s -o %t.asserts.cir
   RUN: wtc-opt %t.asserts.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PARSEC

   CHECK: C queue struct bq lifted (bq_init: init, bq_destroy: destroy, bq_sender_done: terminate, bq_get_many: batched pop, bq_put_many: batched push); capacity: parameter 1 + 0; finished after as many terminations as parameter 2 of init
   CHECK: queue *@first_q: spsc (producers: 1, consumers: 1)
   CHECK: queue *@last_q: spsc (producers: 1, consumers: 1)
   TWO: C queue struct bq lifted (bq_init: init, bq_destroy: destroy, bq_sender_done: terminate, bq_get_many: batched pop, bq_put_many: batched push)
   TWO: queue *@first_q: spmc (producers: 1, consumers: many)
   TWO: queue *@last_q: mpsc (producers: many, consumers: 1)
   TWO: specialised: constructor tag rewritten to spmc
   TWO: specialised: constructor tag rewritten to mpsc
   TSAN-NOT: ThreadSanitizer
   OUT: chunks 30000 sum 449985000
   PARSEC: C queue struct bq not lifted: (d) {{.*}} ends the program when {{.*}}, a check the substitute does not make
*/
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(PARSEC_ASSERTS)
#define CHECK(x) assert(x)
#else
#define CHECK(x) ((void)0)
#endif

struct fifo {
  int wr, rd; /* where the next element goes, where the oldest is */
  void **cell;
  size_t n; /* cells: one more than it holds */
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
  int failed = fifo_init(&q->ring, cap);
  assert(!failed);
  (void)failed;
  q->senders = senders;
  q->closed = 0;
}

void bq_destroy(struct bq *q) {
  pthread_cond_destroy(&q->can_put);
  pthread_cond_destroy(&q->can_get);
  pthread_mutex_destroy(&q->mu);
  fifo_free(&q->ring);
}

static inline int bq_finished(struct bq *q) {
  CHECK(q->closed <= q->senders);
  return q->closed == q->senders;
}

void bq_sender_done(struct bq *q) {
  pthread_mutex_lock(&q->mu);
  q->closed++;
  if (bq_finished(q)) pthread_cond_broadcast(&q->can_get);
  pthread_mutex_unlock(&q->mu);
}

int bq_get_many(struct bq *q, struct fifo *into, int max) {
  int i;
  pthread_mutex_lock(&q->mu);
  while (fifo_empty(&q->ring) && !bq_finished(q)) pthread_cond_wait(&q->can_get, &q->mu);
  if (fifo_empty(&q->ring) && bq_finished(q)) {
    pthread_mutex_unlock(&q->mu);
    return -1;
  }
  for (i = 0; i < max && !fifo_empty(&q->ring) && !fifo_full(into); i++) {
    void *x = fifo_get(&q->ring);
    CHECK(x != NULL);
    int r = fifo_put(into, x);
    assert(r == 0);
    (void)r;
  }
  if (i > 0) pthread_cond_signal(&q->can_put);
  pthread_mutex_unlock(&q->mu);
  return i;
}

int bq_put_many(struct bq *q, struct fifo *from, int max) {
  int i;
  pthread_mutex_lock(&q->mu);
  CHECK(!bq_finished(q));
  while (fifo_full(&q->ring)) pthread_cond_wait(&q->can_put, &q->mu);
  for (i = 0; i < max && !fifo_full(&q->ring) && !fifo_empty(from); i++) {
    void *x = fifo_get(from);
    CHECK(x != NULL);
    int r = fifo_put(&q->ring, x);
    assert(r == 0);
    (void)r;
  }
  if (i > 0) pthread_cond_signal(&q->can_get);
  pthread_mutex_unlock(&q->mu);
  return i;
}

#define CHUNKS 30000
#define BATCH 20
#ifdef TWO
#define MIDDLE 2
#else
#define MIDDLE 1
#endif

struct bq *first_q, *last_q;
long total, count;

void *source(void *arg) {
  (void)arg;
  struct fifo out;
  fifo_init(&out, BATCH);
  for (long i = 0; i < CHUNKS; ++i) {
    long *chunk = (long *)malloc(sizeof(long));
    *chunk = i;
    fifo_put(&out, chunk);
    if (fifo_full(&out)) bq_put_many(&first_q[0], &out, BATCH);
  }
  while (!fifo_empty(&out)) bq_put_many(&first_q[0], &out, BATCH);
  fifo_free(&out);
  bq_sender_done(&first_q[0]);
  return NULL;
}

void *middle(void *arg) {
  (void)arg;
  struct fifo in, out;
  fifo_init(&in, BATCH);
  fifo_init(&out, BATCH);
  for (;;) {
    if (fifo_empty(&in) && bq_get_many(&first_q[0], &in, BATCH) < 0) break;
    long *chunk = (long *)fifo_get(&in);
    *chunk *= 3;
    fifo_put(&out, chunk);
    if (fifo_full(&out)) bq_put_many(&last_q[0], &out, BATCH);
  }
  while (!fifo_empty(&out)) bq_put_many(&last_q[0], &out, BATCH);
  fifo_free(&in);
  fifo_free(&out);
  bq_sender_done(&last_q[0]);
  return NULL;
}

void *sink(void *arg) {
  (void)arg;
  struct fifo in;
  fifo_init(&in, BATCH);
  for (;;) {
    if (fifo_empty(&in) && bq_get_many(&last_q[0], &in, BATCH) < 0) break;
    long *chunk = (long *)fifo_get(&in);
    total += *chunk / 3;
    count++;
    free(chunk);
  }
  fifo_free(&in);
  return NULL;
}

int main(void) {
  first_q = (struct bq *)malloc(sizeof(struct bq));
  last_q = (struct bq *)malloc(sizeof(struct bq));
  bq_init(&first_q[0], 1024, 1);
  bq_init(&last_q[0], 1024, MIDDLE);
  pthread_t src, mid[MIDDLE], snk;
  pthread_create(&src, NULL, source, NULL);
#ifdef TWO
  for (int i = 0; i < MIDDLE; ++i) pthread_create(&mid[i], NULL, middle, NULL);
#else
  pthread_create(&mid[0], NULL, middle, NULL);
#endif
  pthread_create(&snk, NULL, sink, NULL);
  pthread_join(src, NULL);
  for (int i = 0; i < MIDDLE; ++i) pthread_join(mid[i], NULL);
  pthread_join(snk, NULL);
  bq_destroy(&first_q[0]);
  bq_destroy(&last_q[0]);
  free(first_q);
  free(last_q);
  printf("chunks %ld sum %ld\n", count, total);
  return 0;
}
