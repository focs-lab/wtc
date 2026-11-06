/* Once an open finding (docs/lifting.md, "Findings of 1 Nov 2026"): a
   queue inside a union, read through the union's other member. The program
   reads the queue's first two ints (put, take) as the bytes of words[0].
   The holder rules let a pointer to the union go to member steps, and the
   step to words is not a step to the queue. Lifted, words[0] is the
   substitute's pointer. Now a step into a union that holds the queue, to
   another of its members, refuses the struct; the program prints what it
   prints as written.

   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
   RUN: %t.exe | FileCheck %s --match-full-lines

   CHECK: put 3 take 2
   WHY: C queue struct chan not lifted: (a) main reads a union that holds the queue through another of its members
*/
#include "chan.h"

union view {
  struct chan q;
  long words[8];
};
static union view u;

int main(void) {
  chan_open(&u.q, 8, 1);
  for (long i = 1; i <= 3; ++i) chan_send(&u.q, (void *)i);
  void *p;
  chan_recv(&u.q, &p);
  chan_recv(&u.q, &p);
  printf("put %ld take %ld\n", u.words[0] & 0xffffffff, u.words[0] >> 32);
  chan_writer_done(&u.q);
  chan_close(&u.q);
  return 0;
}
