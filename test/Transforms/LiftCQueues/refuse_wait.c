/* (b), (c) A wait releases the mutex it is given, which must be the one
   held, the queue's; and waits on, and wakes, condition variables of the
   queue. A wait with another mutex, or a wake-up of a variable outside the
   queue (which the substitute would drop), is refused.

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DOTHER_MUTEX -o %t.m.cir
   RUN: wtc-opt %t.m.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=MUTEX
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DOTHER_CV -o %t.cv.cir
   RUN: wtc-opt %t.cv.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=CV

   MUTEX: C queue struct chan not lifted: (b) chan_send waits with a mutex other than the one it holds
   CV: C queue struct chan not lifted: (c) chan_send wakes a condition variable outside the queue
*/
#define CUSTOM_SEND
#include "chan.h"

struct watch {
  pthread_mutex_t m;
  pthread_cond_t sent;
};

void chan_send(struct chan *c, void *v, struct watch *w) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
#if defined(OTHER_MUTEX)
    pthread_cond_wait(&c->nonfull, &w->m);
#else
    pthread_cond_wait(&c->nonfull, &c->lock);
#endif
  c->slot[c->put] = v;
  if (++c->put == c->nslots) c->put = 0;
  pthread_cond_signal(&c->nonempty);
#if defined(OTHER_CV)
  pthread_cond_broadcast(&w->sent);
#endif
  pthread_mutex_unlock(&c->lock);
}

struct chan q;
struct watch w;
int main(void) {
  chan_open(&q, 8, 1);
  chan_send(&q, (void *)1, &w);
  chan_writer_done(&q);
  void *p;
  while (chan_recv(&q, &p) == 0)
    ;
  chan_close(&q);
  return 0;
}
