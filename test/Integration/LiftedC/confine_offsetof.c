/* A finding of the review of 6 Nov 2026: one file, nothing linked: main
   reads the queue's index by offsetof arithmetic from another member of
   the struct that holds it. Lifted, the program printed otherwise; now the
   struct is refused, and the program prints what it prints as written.

   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
   RUN: %t.exe | FileCheck %s --match-full-lines

   CHECK: take 2
   WHY: C queue struct chan not lifted: (a) main makes a value that may point to it a pointer to a signed 32-bit integer, and reads a queue as that type
*/
#include "chan.h"
#include <stddef.h>
struct app {
  int id;
  struct chan q;
};
static struct app the_app;
int main(void) {
  chan_open(&the_app.q, 8, 1);
  for (long i = 1; i <= 3; ++i) chan_send(&the_app.q, (void *)i);
  void *p;
  chan_recv(&the_app.q, &p);
  chan_recv(&the_app.q, &p);
  char *base = (char *)&the_app.id - offsetof(struct app, id);
  int take = *(int *)(base + offsetof(struct app, q) + offsetof(struct chan, take));
  printf("take %d\n", take);
  chan_writer_done(&the_app.q);
  chan_close(&the_app.q);
  return 0;
}
