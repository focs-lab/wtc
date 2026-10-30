/* (a) A queue the program initialises statically, `= { ... }`, is never
   built by the substitute's init, and its operations would find no queue
   (it crashed): refused. A global without an initialiser, which init then
   builds, lifts.

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DSTATIC -o %t.cir
   RUN: wtc-opt %t.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -o %t.plain.cir
   RUN: wtc-opt %t.plain.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PLAIN

   CHECK: C queue struct chan not lifted: (a) fixed is initialised statically, not by chan_open
   PLAIN: C queue struct chan lifts
*/
#include "chan.h"

#if defined(STATIC)
static void *buf[4];
struct chan fixed = {0, 0, buf, 4, 1, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
                     PTHREAD_COND_INITIALIZER};
#endif
struct chan q;

int main(void) {
  chan_open(&q, 4, 1);
  void *p;
  chan_send(&q, (void *)1);
  chan_writer_done(&q);
  while (chan_recv(&q, &p) == 0)
    ;
#if defined(STATIC)
  chan_send(&fixed, (void *)2);
  chan_writer_done(&fixed);
  while (chan_recv(&fixed, &p) == 0)
    ;
#endif
  chan_close(&q);
  return 0;
}
