/* (e) The substitute keeps pointers: a ring of another type, even one of
   a pointer's size (long, double), is refused. It used to lift and then
   fail to build.

   RUN: %clang -fclangir -emit-cir %s -DT=long -o %t.long.cir
   RUN: wtc-opt %t.long.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -fclangir -emit-cir %s -DT=double -o %t.double.cir
   RUN: wtc-opt %t.double.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s

   CHECK: C queue struct lq not lifted: (e) the ring slot holds something other than pointers
*/
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct lq {
  int put, take;
  T *slot;
  int nslots;
  pthread_mutex_t lock;
  pthread_cond_t nonempty, nonfull;
};

void lq_open(struct lq *c, int n) {
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->nonempty, NULL);
  pthread_cond_init(&c->nonfull, NULL);
  c->slot = (T *)malloc(sizeof(T) * n);
  c->nslots = n;
  c->put = c->take = 0;
}

void lq_send(struct lq *c, T v) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots) pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
  if (++c->put == c->nslots) c->put = 0;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}

int lq_recv(struct lq *c, T *out) {
  pthread_mutex_lock(&c->lock);
  while (c->take == c->put) pthread_cond_wait(&c->nonempty, &c->lock);
  *out = c->slot[c->take];
  if (++c->take == c->nslots) c->take = 0;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}

struct lq q;
int main(void) {
  lq_open(&q, 4);
  T s = 0, v;
  for (int i = 1; i <= 3; ++i) lq_send(&q, i);
  for (int i = 0; i < 3; ++i) {
    lq_recv(&q, &v);
    s += v;
  }
  printf("sum %g\n", (double)s);
  return 0;
}
