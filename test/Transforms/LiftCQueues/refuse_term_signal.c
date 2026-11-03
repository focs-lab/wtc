/* (d) The last termination must wake every waiting pop. The substitute's
   counted close does; a pthread_cond_signal wakes one, and the others
   sleep on unless the program passes the wake-up on, which the pass does
   not follow through the pops' callers. With two readers asleep and a pop
   that does not pass it on, the original leaves one asleep for ever, and
   the lifted program finished. Refused; with pthread_cond_broadcast
   (chan.h's own), it lifts.

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DSIGNAL -o %t.cir
   RUN: wtc-opt %t.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -o %t.plain.cir
   RUN: wtc-opt %t.plain.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PLAIN

   CHECK: C queue struct chan not lifted: (d) chan_writer_done wakes one pop at the last termination, with a signal, where the substitute wakes them all
   PLAIN: C queue struct chan lifts
*/
#if defined(SIGNAL)
#define CUSTOM_DONE
#endif
#include "chan.h"

#if defined(SIGNAL)
void chan_writer_done(struct chan *c) {
  pthread_mutex_lock(&c->lock);
  c->done++;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}
#endif

struct chan q;

static void *reader(void *a) {
  void *p;
  while (chan_recv(&q, &p) == 0) {
  }
  return a;
}

int main(void) {
  chan_open(&q, 8, 1);
  pthread_t r1, r2;
  pthread_create(&r1, NULL, reader, NULL);
  pthread_create(&r2, NULL, reader, NULL);
  chan_writer_done(&q);
  pthread_join(r1, NULL);
  pthread_join(r2, NULL);
  chan_close(&q);
  return 0;
}
