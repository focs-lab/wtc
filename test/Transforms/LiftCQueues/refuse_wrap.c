/* (e) An index wraps at the slot count initialisation sets: one that
   wraps at a constant of its own (here 4, in a ring of 8 slots) is a
   different queue, and is refused; the pass read only that the index
   advanced by one.

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DPOP -o %t.pop.cir
   RUN: wtc-opt %t.pop.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=POP

   CHECK: C queue struct chan not lifted: (e) chan_send wraps the index at 4, not at the slot count nslots
   POP: C queue struct chan not lifted: (e) chan_recv wraps the index at 4, not at the slot count nslots
*/
#if defined(POP)
#define CUSTOM_RECV
#else
#define CUSTOM_SEND
#endif
#include "chan.h"

#if defined(POP)
int chan_recv(struct chan *c, void **out) {
  pthread_mutex_lock(&c->lock);
  while (c->take == c->put && c->done < c->writers)
    pthread_cond_wait(&c->nonempty, &c->lock);
  if (c->take == c->put && c->done == c->writers) {
    pthread_mutex_unlock(&c->lock);
    return -1;
  }
  *out = c->slot[c->take];
  c->take = (c->take + 1) % 4;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}
#else
void chan_send(struct chan *c, void *v) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
    pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
  c->put = (c->put + 1) % 4;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}
#endif

struct chan q;
int main(void) {
  chan_open(&q, 8, 1);
  for (long i = 1; i <= 6; ++i) chan_send(&q, (void *)i);
  chan_writer_done(&q);
  void *p;
  while (chan_recv(&q, &p) == 0) printf("%ld ", (long)p);
  printf("end\n");
  chan_close(&q);
  return 0;
}
