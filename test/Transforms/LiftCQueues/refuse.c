/* What --wtc-lift-c-queues refuses, one struct per condition of the proof
   (docs/lifting.md, "C queues"), each otherwise a queue that lifts: the
   first struct is the shape that does, for comparison.

   RUN: %clang -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s

   CHECK-DAG: C queue struct good lifts (good_init: init, good_end: terminate, good_push: push, good_pop: pop)
   CHECK-DAG: C queue struct seen not lifted: (a) main touches head
   CHECK-DAG: C queue struct early not lifted: (b) early_pop touches the queue without its mutex
   CHECK-DAG: C queue struct counted not lifted: (c) counted_push writes pushes, which the mutex guards too
   CHECK-DAG: C queue struct nowait not lifted: (d) nowait_push does not wait while the ring is full, or waits twice
   CHECK-DAG: C queue struct giveup not lifted: (d) giveup_pop decides on (0 == q->ended (after the wait))
   CHECK-DAG: C queue struct timed not lifted: (d) timed_pop waits with a time limit
   CHECK-DAG: C queue struct stack not lifted: (e) stack_pop reaches elements other than the ring's
   CHECK-DAG: C queue struct quiet not lifted: (d) quiet_push does not wake the pops
   CHECK-DAG: C queue struct sized not lifted: (a) sized_count takes the mutex but is none of the queue's operations
*/
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#define QUEUE(name, extra)                                                     \
  struct name {                                                                \
    int head, tail;                                                            \
    void **data;                                                               \
    int size, producers, ended;                                                \
    extra                                                                      \
    pthread_mutex_t m;                                                         \
    pthread_cond_t empty, full;                                                \
  };                                                                           \
  void name##_init(struct name *q, int size, int producers) {                  \
    pthread_mutex_init(&q->m, NULL);                                           \
    pthread_cond_init(&q->empty, NULL);                                        \
    pthread_cond_init(&q->full, NULL);                                         \
    q->head = q->tail = 0;                                                     \
    q->data = (void **)malloc(sizeof(void *) * size);                          \
    q->size = size;                                                            \
    q->producers = producers;                                                  \
    q->ended = 0;                                                              \
  }                                                                            \
  void name##_end(struct name *q) {                                            \
    pthread_mutex_lock(&q->m);                                                 \
    q->ended++;                                                                \
    pthread_cond_broadcast(&q->empty);                                         \
    pthread_mutex_unlock(&q->m);                                               \
  }

#define PUSH_BODY                                                              \
  pthread_mutex_lock(&q->m);                                                   \
  while (q->head == (q->tail - 1 + q->size) % q->size)                         \
    pthread_cond_wait(&q->full, &q->m);                                        \
  q->data[q->head] = v;                                                        \
  q->head = (q->head + 1) % q->size;                                           \
  pthread_cond_signal(&q->empty);                                              \
  pthread_mutex_unlock(&q->m);

#define POP_BODY                                                               \
  pthread_mutex_lock(&q->m);                                                   \
  while (q->tail == q->head && q->ended < q->producers)                        \
    pthread_cond_wait(&q->empty, &q->m);                                       \
  if (q->tail == q->head) {                                                    \
    pthread_mutex_unlock(&q->m);                                               \
    return -1;                                                                 \
  }                                                                            \
  *out = q->data[q->tail];                                                     \
  q->tail = (q->tail + 1) % q->size;                                           \
  pthread_cond_signal(&q->full);                                               \
  pthread_mutex_unlock(&q->m);                                                 \
  return 0;

QUEUE(good, )
void good_push(struct good *q, void *v) { PUSH_BODY }
int good_pop(struct good *q, void **out) { POP_BODY }

/* (a) main reads a member itself. */
QUEUE(seen, )
void seen_push(struct seen *q, void *v) { PUSH_BODY }
int seen_pop(struct seen *q, void **out) { POP_BODY }

/* (b) a look at the ring before the lock. */
QUEUE(early, )
void early_push(struct early *q, void *v) { PUSH_BODY }
int early_pop(struct early *q, void **out) {
  if (q->tail == q->head && q->ended == q->producers) return -1;
  POP_BODY
}

/* (c) a count of pushes under the same mutex. */
QUEUE(counted, int pushes;)
void counted_push(struct counted *q, void *v) {
  pthread_mutex_lock(&q->m);
  while (q->head == (q->tail - 1 + q->size) % q->size)
    pthread_cond_wait(&q->full, &q->m);
  q->data[q->head] = v;
  q->head = (q->head + 1) % q->size;
  q->pushes++;
  pthread_cond_signal(&q->empty);
  pthread_mutex_unlock(&q->m);
}
int counted_pop(struct counted *q, void **out) { POP_BODY }

/* (d) a push that writes over a full ring rather than waiting. */
QUEUE(nowait, )
void nowait_push(struct nowait *q, void *v) {
  pthread_mutex_lock(&q->m);
  q->data[q->head] = v;
  q->head = (q->head + 1) % q->size;
  pthread_cond_signal(&q->empty);
  pthread_mutex_unlock(&q->m);
}
int nowait_pop(struct nowait *q, void **out) { POP_BODY }

/* (d) a pop that stops waiting at the first termination. */
QUEUE(giveup, )
void giveup_push(struct giveup *q, void *v) { PUSH_BODY }
int giveup_pop(struct giveup *q, void **out) {
  pthread_mutex_lock(&q->m);
  while (q->tail == q->head && q->ended == 0)
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

/* (d) a pop with a time limit. */
QUEUE(timed, )
void timed_push(struct timed *q, void *v) { PUSH_BODY }
int timed_pop(struct timed *q, void **out) {
  struct timespec ts = {0, 0};
  pthread_mutex_lock(&q->m);
  while (q->tail == q->head && q->ended < q->producers)
    pthread_cond_timedwait(&q->empty, &q->m, &ts);
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

/* (e) the newest element first: a stack. */
QUEUE(stack, )
void stack_push(struct stack *q, void *v) { PUSH_BODY }
int stack_pop(struct stack *q, void **out) {
  pthread_mutex_lock(&q->m);
  while (q->tail == q->head && q->ended < q->producers)
    pthread_cond_wait(&q->empty, &q->m);
  if (q->tail == q->head) {
    pthread_mutex_unlock(&q->m);
    return -1;
  }
  q->head = (q->head - 1 + q->size) % q->size;
  *out = q->data[q->head];
  pthread_cond_signal(&q->full);
  pthread_mutex_unlock(&q->m);
  return 0;
}

/* (d) a push that wakes nobody. */
QUEUE(quiet, )
void quiet_push(struct quiet *q, void *v) {
  pthread_mutex_lock(&q->m);
  while (q->head == (q->tail - 1 + q->size) % q->size)
    pthread_cond_wait(&q->full, &q->m);
  q->data[q->head] = v;
  q->head = (q->head + 1) % q->size;
  pthread_mutex_unlock(&q->m);
}
int quiet_pop(struct quiet *q, void **out) { POP_BODY }

/* (a) a count read under the mutex: no operation of the substitute's. */
QUEUE(sized, )
void sized_push(struct sized *q, void *v) { PUSH_BODY }
int sized_pop(struct sized *q, void **out) { POP_BODY }
int sized_count(struct sized *q) {
  pthread_mutex_lock(&q->m);
  int n = (q->head - q->tail + q->size) % q->size;
  pthread_mutex_unlock(&q->m);
  return n;
}

struct good g;
struct seen s;
struct early e;
struct counted c;
struct nowait n;
struct giveup gu;
struct timed t;
struct stack st;
struct quiet qu;
struct sized sz;

int main(void) {
  void *x;
  int one = 1;
  good_init(&g, 4, 1), good_push(&g, &one), good_end(&g), good_pop(&g, &x);
  seen_init(&s, 4, 1), seen_push(&s, &one), seen_end(&s), seen_pop(&s, &x);
  early_init(&e, 4, 1), early_push(&e, &one), early_end(&e), early_pop(&e, &x);
  counted_init(&c, 4, 1), counted_push(&c, &one), counted_end(&c), counted_pop(&c, &x);
  nowait_init(&n, 4, 1), nowait_push(&n, &one), nowait_end(&n), nowait_pop(&n, &x);
  giveup_init(&gu, 4, 1), giveup_push(&gu, &one), giveup_end(&gu), giveup_pop(&gu, &x);
  timed_init(&t, 4, 1), timed_push(&t, &one), timed_end(&t), timed_pop(&t, &x);
  stack_init(&st, 4, 1), stack_push(&st, &one), stack_end(&st), stack_pop(&st, &x);
  quiet_init(&qu, 4, 1), quiet_push(&qu, &one), quiet_end(&qu), quiet_pop(&qu, &x);
  sized_init(&sz, 4, 1), sized_push(&sz, &one), sized_end(&sz), sized_pop(&sz, &x), sized_count(&sz);
  return s.head;
}
