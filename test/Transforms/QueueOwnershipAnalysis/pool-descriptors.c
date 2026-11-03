/* Threads started by a pool from descriptors, as PARSEC ferret starts its
   stages (src/tpool.c): main fills an array of {attr, entry, arg} per
   stage and hands it with its length to a function that starts one thread
   per descriptor in a loop. The entries were functions whose address is
   taken, so each queue's sides were any number of threads, and the loop's
   count was a parameter of a function the queues are not built in.

   Now (default): an entry stored only into a field that is only ever
   compared or handed to a spawn as its entry is no address taken; the
   spawn reads the field back from the array the call hands it, so each
   stage's threads run their own entry; and the loop's count is the value
   main passes for it, read where the queue is built, so each queue's tag
   is chosen there.

   What must not pass:
   CALLED  the field is also called directly: its functions may run on
           any thread, any number of times.
   LEAKED  a descriptor is handed to a function with no body here, which
           may start or call it.
   MIXED   one array holds two entries: each thread of that pool may run
           either, and both are counted on every queue they touch.
   WRITTEN the count is written after the queue is built: the value at the
           constructor is not the loop's.
   CHANGED the pool changes its count before the loop.
   TWO     two pools start the producers of one queue: no one call's count
           is theirs.

   RUN: %clang -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --mlir-print-op-on-diagnostic=false --wtc-lift-c-queues --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
   CHECK-NOT: address of this function is taken
   CHECK: queue @to_work: producers: one thread per iteration of the loop at {{.*}}, run by the call at pool-descriptors.c:{{[0-9]+}}
   CHECK: queue @to_work: consumers: one thread per iteration of the loop at {{.*}}, run by the call at pool-descriptors.c:{{[0-9]+}}
   CHECK: specialised at run time: spsc when the loops at {{.*}} (producers) and {{.*}} (consumers) each run at most once

   RUN: %clang -fclangir -emit-cir -DCALLED %s -o %t.called.cir
   RUN: wtc-opt %t.called.cir --mlir-print-op-on-diagnostic=false --wtc-lift-c-queues --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAKEN
   RUN: %clang -fclangir -emit-cir -DLEAKED %s -o %t.leaked.cir
   RUN: wtc-opt %t.leaked.cir --mlir-print-op-on-diagnostic=false --wtc-lift-c-queues --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=TAKEN
   TAKEN: address of this function is taken
   TAKEN-NOT: specialised at run time

   RUN: %clang -fclangir -emit-cir -DMIXED %s -o %t.mixed.cir
   RUN: wtc-opt %t.mixed.cir --mlir-print-op-on-diagnostic=false --wtc-lift-c-queues --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=MIXED
   MIXED-NOT: address of this function is taken
   MIXED: queue @to_work: mpmc (producers: many, consumers: many)
   MIXED-NOT: queue @to_work: producers: one thread per iteration

   RUN: %clang -fclangir -emit-cir -DWRITTEN %s -o %t.written.cir
   RUN: wtc-opt %t.written.cir --mlir-print-op-on-diagnostic=false --wtc-lift-c-queues --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=WRITTEN
   WRITTEN: not specialised at run time: the loop's bound may be written after the queue is built
   WRITTEN-NOT: specialised at run time: spsc

   RUN: %clang -fclangir -emit-cir -DCHANGED %s -o %t.changed.cir
   RUN: wtc-opt %t.changed.cir --mlir-print-op-on-diagnostic=false --wtc-lift-c-queues --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=CHANGED
   CHANGED: not specialised at run time: the loop's bound is not a parameter its function keeps as it is
   CHANGED-NOT: specialised at run time: spsc

   RUN: %clang -fclangir -emit-cir -DTWO %s -o %t.two.cir
   RUN: wtc-opt %t.two.cir --mlir-print-op-on-diagnostic=false --wtc-lift-c-queues --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=TWO
   TWO: queue @to_work: mpmc (producers: many, consumers: many)
   TWO-NOT: queue @to_work: producers: one thread per iteration
*/
#include <pthread.h>
#include <stdlib.h>

struct chan {
  int put, take;
  void **slot;
  int nslots;
  int writers;
  int done;
  pthread_mutex_t lock;
  pthread_cond_t nonempty, nonfull;
};

void chan_open(struct chan *c, int nslots, int writers) {
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->nonempty, NULL);
  pthread_cond_init(&c->nonfull, NULL);
  c->slot = (void **)malloc(sizeof(void *) * nslots);
  c->nslots = nslots;
  c->writers = writers;
  c->put = c->take = c->done = 0;
}

void chan_writer_done(struct chan *c) {
  pthread_mutex_lock(&c->lock);
  c->done++;
  pthread_cond_broadcast(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}

int chan_recv(struct chan *c, void **out) {
  pthread_mutex_lock(&c->lock);
  while (c->take == c->put && c->done < c->writers)
    pthread_cond_wait(&c->nonempty, &c->lock);
  if (c->take == c->put && c->done == c->writers) {
    pthread_cond_broadcast(&c->nonempty);
    pthread_mutex_unlock(&c->lock);
    return -1;
  }
  *out = c->slot[c->take];
  if (++c->take == c->nslots) c->take = 0;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}

void chan_send(struct chan *c, void *v) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
    pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
  if (++c->put == c->nslots) c->put = 0;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}

struct chan to_work;
long total;

void *loader(void *arg) {
  (void)arg;
  for (long i = 1; i <= 100; ++i) chan_send(&to_work, (void *)i);
  chan_writer_done(&to_work);
  return NULL;
}

void *worker(void *arg) {
  (void)arg;
  void *item;
  while (chan_recv(&to_work, &item) == 0) __atomic_fetch_add(&total, (long)item, __ATOMIC_RELAXED);
  return NULL;
}

typedef void *(*entry_fn)(void *);
struct desc {
  const pthread_attr_t *attr;
  entry_fn start;
  void *arg;
};
struct pool {
  int n;
  pthread_t *ids;
};

#ifdef LEAKED
void inspect(struct desc *d);
#endif

struct pool *pool_start(struct desc *d, int n) {
  if (d == NULL || n < 1) return NULL;
  for (int i = 0; i < n; i++)
    if (d[i].start == NULL) return NULL;
#ifdef CHANGED
  n = n * 2 - 1;
#endif
  struct pool *p = (struct pool *)malloc(sizeof(struct pool));
  p->ids = (pthread_t *)malloc(sizeof(pthread_t) * n);
  for (int i = 0; i < n; i++)
    if (pthread_create(&p->ids[i], d[i].attr, d[i].start, d[i].arg) != 0) return NULL;
#ifdef CALLED
  d[0].start(NULL);
#endif
  p->n = n;
  return p;
}

void pool_join(struct pool *p) {
  for (int i = 0; i < p->n; i++) pthread_join(p->ids[i], NULL);
  free(p->ids);
  free(p);
}

int n_load = 1, n_work = 1;

int main(int argc, char **argv) {
  n_work = argc > 1 ? atoi(argv[1]) : 1;
  chan_open(&to_work, 20, n_load);
#ifdef WRITTEN
  n_work = n_work + 1;
#endif
  struct desc *dl = (struct desc *)calloc(n_load, sizeof(struct desc));
  struct desc *dw = (struct desc *)calloc(n_work, sizeof(struct desc));
  dl[0].attr = NULL;
  dl[0].start = loader;
  dl[0].arg = NULL;
  dw[0].attr = NULL;
  dw[0].start = worker;
  dw[0].arg = NULL;
  for (int i = 1; i < n_work; i++) dw[i] = dw[0];
#ifdef MIXED
  if (n_work > 1) dw[1].start = loader;
#endif
#ifdef LEAKED
  inspect(dw);
#endif
  struct pool *pl = pool_start(dl, n_load);
#ifdef TWO
  struct pool *pl2 = pool_start(dl, n_load);
#endif
  struct pool *pw = pool_start(dw, n_work);
  pool_join(pl);
#ifdef TWO
  pool_join(pl2);
#endif
  pool_join(pw);
  free(dl);
  free(dw);
  return total == 5050 ? 0 : 1;
}
