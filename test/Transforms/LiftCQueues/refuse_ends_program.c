/* (d) A branch on which an operation ends the program (abort, exit, a
   failed assertion) is a check the substitute does not make: refused,
   whatever the branch reads, a parameter or the queue. The one allowed
   is an initialisation's on its own allocation, which the substitute's
   allocation makes as well (it ends the program when it fails).

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DPARAM -o %t.param.cir
   RUN: wtc-opt %t.param.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PARAM
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DSTATE -o %t.state.cir
   RUN: wtc-opt %t.state.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=STATE
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DALLOC -o %t.alloc.cir
   RUN: wtc-opt %t.alloc.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=ALLOC

   PARAM: C queue struct chan not lifted: (d) chan_send ends the program when (parameter 1 == null), a check the substitute does not make
   STATE: C queue struct chan not lifted: (d) chan_send ends the program when ((1 + q->done) == q->writers)
   ALLOC: C queue struct chan lifts
*/
#include <assert.h>
#define CUSTOM_SEND
#define CUSTOM_OPEN
#include "chan.h"

void chan_open(struct chan *c, int nslots, int writers) {
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->nonempty, NULL);
  pthread_cond_init(&c->nonfull, NULL);
  c->slot = (void **)malloc(sizeof(void *) * nslots);
  assert(c->slot != NULL);
  c->nslots = nslots;
  c->writers = writers;
  c->put = c->take = c->done = 0;
}

void chan_send(struct chan *c, void *v) {
  pthread_mutex_lock(&c->lock);
#if defined(PARAM)
  if (v == NULL) abort();
#elif defined(STATE)
  /* sending after the last writer finished */
  assert(c->done + 1 != c->writers);
#endif
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
    pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
  if (++c->put == c->nslots) c->put = 0;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}

struct chan q;
int main(void) {
  chan_open(&q, 8, 1);
  chan_send(&q, (void *)1);
  chan_writer_done(&q);
  void *p;
  while (chan_recv(&q, &p) == 0)
    ;
  chan_close(&q);
  return 0;
}
