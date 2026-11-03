/* (c) Memory outside the queue: a pop writes only the element it hands to
   its caller, on the path that takes one. A pop that also clears *out
   when the queue is finished, or reports through a second parameter,
   would lose those writes with its body, and is refused.

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DNULL_OUT -o %t.null.cir
   RUN: wtc-opt %t.null.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=NULL
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DSTATUS -o %t.status.cir
   RUN: wtc-opt %t.status.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=STATUS
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DCOUNTER -o %t.counter.cir
   RUN: wtc-opt %t.counter.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=COUNTER

   NULL: C queue struct chan not lifted: (c) chan_recv writes *parameter 1, outside the queue, when the queue is finished
   STATUS: C queue struct chan not lifted: (c) chan_recv writes *parameter 2, outside the queue
   COUNTER: C queue struct chan not lifted: (c) chan_send writes *parameter 2, outside the queue
*/
#define CUSTOM_RECV
#if defined(COUNTER)
#define CUSTOM_SEND
#endif
#include "chan.h"

#if defined(STATUS)
int chan_recv(struct chan *c, void **out, int *got) {
#else
int chan_recv(struct chan *c, void **out) {
#endif
  pthread_mutex_lock(&c->lock);
  while (c->take == c->put && c->done < c->writers)
    pthread_cond_wait(&c->nonempty, &c->lock);
  if (c->take == c->put && c->done == c->writers) {
#if defined(NULL_OUT)
    *out = NULL;
#elif defined(STATUS)
    *got = 0;
#endif
    pthread_cond_broadcast(&c->nonempty);
    pthread_mutex_unlock(&c->lock);
    return -1;
  }
  *out = c->slot[c->take];
#if defined(STATUS)
  *got = 1;
#endif
  if (++c->take == c->nslots) c->take = 0;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}

#if defined(COUNTER)
void chan_send(struct chan *c, void *v, long *sent) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
    pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
  if (++c->put == c->nslots) c->put = 0;
  *sent = *sent + 1;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}
#endif

struct chan q;
int main(void) {
  chan_open(&q, 8, 1);
#if defined(COUNTER)
  long sent = 0;
  chan_send(&q, (void *)7, &sent);
#else
  chan_send(&q, (void *)7);
#endif
  chan_writer_done(&q);
  void *p;
#if defined(STATUS)
  int got;
  while (chan_recv(&q, &p, &got) == 0)
    ;
#else
  while (chan_recv(&q, &p) == 0)
    ;
#endif
  chan_close(&q);
  return 0;
}
