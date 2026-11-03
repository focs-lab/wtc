/* (a) A pointer to the queue stored where the pass does not follow it,
   here a member of a global struct that another file is handed, reaches
   code that touches the queue's members: refused. Stored in a local or in
   a global of the pointer's own type, whose every use the pass sees, it
   lifts.

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DHOLDER -o %t.cir
   RUN: wtc-opt %t.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -o %t.plain.cir
   RUN: wtc-opt %t.plain.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PLAIN

   CHECK: C queue struct chan not lifted: (a) main stores a pointer to the struct in memory the pass does not follow
   PLAIN: C queue struct chan lifts
*/
#include "chan.h"

struct holder {
  struct chan *c;
};
int peek_take(struct holder *h); /* in another file, linked with this one */
struct chan q;
struct holder h;
struct chan *current;

int main(void) {
  struct chan *local = &q;
  current = &q;
  chan_open(local, 8, 1);
  for (long i = 1; i <= 3; ++i) chan_send(current, (void *)i);
  void *p;
  chan_recv(&q, &p);
#if defined(HOLDER)
  h.c = &q;
  printf("progress %d\n", peek_take(&h));
#endif
  chan_writer_done(&q);
  chan_close(&q);
  return 0;
}
