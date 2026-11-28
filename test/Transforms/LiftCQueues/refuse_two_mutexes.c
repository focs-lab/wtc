/* (b) Every path of an operation holds one mutex, the struct's, from its
   first touch of the queue to its last. A push that takes one of two
   mutexes of the struct, by a parameter, holds the queue under either:
   two pushes, one on each path, would not exclude each other, which the
   substitute's would.

   RUN: %clang -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s

   CHECK: C queue struct two not lifted: (b) two_push locks two mutexes
*/
#include <pthread.h>
#include <stdlib.h>
struct two {
  int head, tail;
  void **data;
  int size, producers, ended;
  pthread_mutex_t m, m2;
  pthread_cond_t empty, full;
};
void two_init(struct two *q, int size, int producers) {
  pthread_mutex_init(&q->m, NULL);
  pthread_mutex_init(&q->m2, NULL);
  pthread_cond_init(&q->empty, NULL);
  pthread_cond_init(&q->full, NULL);
  q->head = q->tail = 0;
  q->data = (void **)malloc(sizeof(void *) * size);
  q->size = size;
  q->producers = producers;
  q->ended = 0;
}
void two_end(struct two *q) {
  pthread_mutex_lock(&q->m);
  q->ended++;
  pthread_cond_broadcast(&q->empty);
  pthread_mutex_unlock(&q->m);
}
#define PUSH_WITH(M)                                                           \
  pthread_mutex_lock(&q->M);                                                   \
  while (q->head == (q->tail - 1 + q->size) % q->size)                         \
    pthread_cond_wait(&q->full, &q->M);                                        \
  q->data[q->head] = v;                                                        \
  q->head = (q->head + 1) % q->size;                                           \
  pthread_cond_signal(&q->empty);                                              \
  pthread_mutex_unlock(&q->M);
void two_push(struct two *q, void *v, int alt) {
  if (alt) {
    PUSH_WITH(m2)
  } else {
    PUSH_WITH(m)
  }
}
int two_pop(struct two *q, void **out) {
  pthread_mutex_lock(&q->m);
  while (q->tail == q->head && q->ended < q->producers)
    pthread_cond_wait(&q->empty, &q->m);
  if (q->tail == q->head) {
    pthread_mutex_unlock(&q->m);
    return -1;
  }
  *out = q->data[q->tail];
  q->tail = (q->tail + 1) % q->size;
  pthread_cond_signal(&q->full);
  pthread_mutex_unlock(&q->m);
  return 0;
}
struct two t;
int main(void) {
  void *p;
  two_init(&t, 4, 1);
  two_push(&t, (void *)1, 0);
  two_end(&t);
  two_pop(&t, &p);
  return 0;
}
