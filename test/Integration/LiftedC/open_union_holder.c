/* Known open finding (docs/lifting.md, "Known open findings"): a queue
   inside a union, read through the union's other member. The program
   reads the queue's first two ints (put, take) as the bytes of words[0].
   The holder rules let a pointer to the union go to member steps, and the
   step to words is not a step to the queue. Lifted, words[0] is the
   substitute's pointer. The test passes once the struct is refused here,
   or lifted keeps what the program prints as written.

   XFAIL: *
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.exe
   RUN: %t.exe | FileCheck %s --match-full-lines

   CHECK: put 3 take 2
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
