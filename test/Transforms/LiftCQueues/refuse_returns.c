/* What an initialisation or a termination returns is what the program
   sees; the substitute's return a constant, so a value computed from the
   queue, or from its allocation, is refused. Without the refusal the
   pass replaced init's result by 0 (the program took its queue for
   unallocated) and dropped terminate's ("the last producer" never seen).

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DINIT -o %t.init.cir
   RUN: wtc-opt %t.init.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=INIT
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DDONE -o %t.done.cir
   RUN: wtc-opt %t.done.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=DONE
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DCONST -o %t.const.cir
   RUN: wtc-opt %t.const.cir --wtc-lift-c-queues -o - 2>&1 | FileCheck %s --check-prefix=CONST

   INIT: C queue struct chan not lifted: (a) chan_open returns ite((malloc((8 * parameter 1)) != null), 1, 0), not a constant
   DONE: C queue struct chan not lifted: (a) chan_writer_done returns ite(((1 + q->done) == q->writers), 1, 0), not a constant
   CONST: C queue struct chan lifted
   CONST-LABEL: cir.func {{.*}}@chan_open(
   CONST: cir.call @wtc_c_queue_init.chan(
   CONST: %[[K:.*]] = cir.const #cir.int<7> : !s32i
   CONST: cir.return %[[K]]
   CONST-LABEL: cir.func {{.*}}@chan_writer_done(
   CONST: cir.call @wtc_c_queue_terminate.chan(
   CONST: %[[D:.*]] = cir.const #cir.int<3> : !s32i
   CONST: cir.return %[[D]]
   CONST-LABEL: cir.func {{.*}}@main(
   CONST: %[[R:.*]] = cir.const #cir.int<7> : !s32i
   CONST: cir.store {{.*}}%[[R]]
*/
#define CUSTOM_OPEN
#define CUSTOM_DONE
#include "chan.h"

int chan_open(struct chan *c, int nslots, int writers) {
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->nonempty, NULL);
  pthread_cond_init(&c->nonfull, NULL);
  c->slot = (void **)malloc(sizeof(void *) * nslots);
  c->nslots = nslots;
  c->writers = writers;
  c->put = c->take = c->done = 0;
#if defined(INIT)
  return c->slot != NULL;
#else
  return 7;
#endif
}

int chan_writer_done(struct chan *c) {
  pthread_mutex_lock(&c->lock);
  c->done++;
#if defined(DONE)
  int last = c->done == c->writers;
#else
  int last = 3;
#endif
  pthread_cond_broadcast(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
  return last;
}

struct chan q;
volatile int opened;
int main(void) {
  opened = chan_open(&q, 4, 1);
  chan_send(&q, (void *)1);
  chan_writer_done(&q);
  void *p;
  while (chan_recv(&q, &p) == 0)
    ;
  chan_close(&q);
  return 0;
}
