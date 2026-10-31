/* What --wtc-lift-c-queues writes: the queue built where init is called,
   with the tag the specialisation pass rewrites and the ring's capacity;
   the operations' bodies replaced by calls through functions of the
   struct's own, which carry the annotations the lifting pass raises and
   call the runtime of shim/c_queue.h; then the raised operations and the
   specialisation.

   RUN: %clang -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-lift-c-queues -o - 2>/dev/null | FileCheck %s
   RUN: wtc-opt %t.cir --mlir-print-op-on-diagnostic=false --wtc-lift-c-queues --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o - 2>&1 | FileCheck %s --check-prefix=SPEC

   CHECK-LABEL: cir.func {{.*}}@push(
   CHECK: cir.call @wtc_c_queue_push_wait.ring(
   CHECK-NEXT: cir.return
   CHECK-LABEL: cir.func {{.*}}@pop(
   CHECK: %[[GOT:.*]] = cir.call @wtc_c_queue_pop_wait.ring(
   CHECK: cir.select if
   CHECK-LABEL: cir.func {{.*}}@finish(
   CHECK: cir.call @wtc_c_queue_terminate.ring(
   CHECK-LABEL: cir.func {{.*}}@main(
   CHECK-NOT: cir.call @init(
   CHECK: %[[CAP:.*]] = cir.add
   CHECK: %[[TAG:.*]] = cir.const #cir.int<0> : !s32i
   CHECK: cir.call @wtc_c_queue_init.ring(%{{.*}}, %[[TAG]], %[[CAP]], %{{.*}})
   CHECK-DAG: cir.func private @wtc_c_queue_pop_wait(!cir.ptr<!void>, !cir.ptr<!cir.ptr<!void>>) -> !s32i
   CHECK-DAG: cir.func internal private @wtc_c_queue_pop_wait.ring({{.*}}) -> !s32i [#cir.annotation<"wtc_queue_pop_wait">, #cir.annotation<"wtc_queue_bounded">, #cir.annotation<"wtc_lifted_c_pop">]
   CHECK-DAG: cir.func internal private @wtc_c_queue_init.ring({{.*}}) [#cir.annotation<"wtc_queue_construction">, #cir.annotation<"wtc_lifted_c_init">]
   CHECK-NOT: wtc_c_queue_push_batch

   SPEC: remark: wtc: C queue struct ring lifted (init: init, push: push, pop: pop, finish: terminate); capacity: parameter 1 - 1; finished after as many terminations as parameter 2 of init
   SPEC: remark: wtc: queue @r: spsc (producers: 1, consumers: 1)
   SPEC: remark: wtc: specialised: constructor tag rewritten to spsc
   SPEC: "wtc.queue_push"({{.*}}bounded{{.*}}impl = #wtc.impl<spsc>{{.*}}wait
   SPEC: "wtc.queue_pop"({{.*}}bounded{{.*}}impl = #wtc.impl<spsc>{{.*}}transfer, wait
*/
#include <pthread.h>
#include <stdlib.h>

struct ring {
  int in, out;
  void **slot;
  int n, producers, done;
  pthread_mutex_t m;
  pthread_cond_t not_empty, not_full;
};

void init(struct ring *r, int n, int producers) {
  pthread_mutex_init(&r->m, NULL);
  pthread_cond_init(&r->not_empty, NULL);
  pthread_cond_init(&r->not_full, NULL);
  r->slot = (void **)malloc(sizeof(void *) * n);
  r->n = n;
  r->in = r->out = r->done = 0;
  r->producers = producers;
}

void push(struct ring *r, void *v) {
  pthread_mutex_lock(&r->m);
  while ((r->in + 1) % r->n == r->out) pthread_cond_wait(&r->not_full, &r->m);
  r->slot[r->in] = v;
  r->in = (r->in + 1) % r->n;
  pthread_cond_signal(&r->not_empty);
  pthread_mutex_unlock(&r->m);
}

int pop(struct ring *r, void **v) {
  pthread_mutex_lock(&r->m);
  while (r->in == r->out && r->done < r->producers) pthread_cond_wait(&r->not_empty, &r->m);
  if (r->in == r->out) {
    pthread_mutex_unlock(&r->m);
    return 0;
  }
  *v = r->slot[r->out];
  r->out = (r->out + 1) % r->n;
  pthread_cond_signal(&r->not_full);
  pthread_mutex_unlock(&r->m);
  return 1;
}

void finish(struct ring *r) {
  pthread_mutex_lock(&r->m);
  r->done++;
  if (r->done == r->producers) pthread_cond_broadcast(&r->not_empty);
  pthread_mutex_unlock(&r->m);
}

struct ring r;
long sum;

void *producer(void *arg) {
  for (long i = 1; i <= 100; ++i) push(&r, (void *)i);
  finish(&r);
  return arg;
}

void *consumer(void *arg) {
  void *v;
  while (pop(&r, &v)) sum += (long)v;
  return arg;
}

int main(void) {
  pthread_t p, c;
  init(&r, 8, 1);
  pthread_create(&p, NULL, producer, NULL);
  pthread_create(&c, NULL, consumer, NULL);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  return sum == 5050 ? 0 : 1;
}
