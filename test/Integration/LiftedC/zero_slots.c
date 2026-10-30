/* A ring whose slot count is read at run time. With zero slots the
   original never pushes, and its pop answers "finished" once the producer
   has terminated. The substitute was built with the slots minus one
   places, and the lifted program ended at init; a ring of no slots is now
   built with no room, and the lifted program prints what the original
   prints at 0 and 1. A slot count below zero ends the program at init
   (docs/lifting.md, C queues), where the original allocates a size that
   wraps.

   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.exe 2>&1 | FileCheck %s
   RUN: %t.exe 0 | FileCheck %s --check-prefix=OUT
   RUN: %t.exe 1 | FileCheck %s --check-prefix=OUT
   RUN: %wtc_cc -O1 --no-lift -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.orig.exe
   RUN: %t.orig.exe 0 | FileCheck %s --check-prefix=OUT
   RUN: not --crash %t.exe -1 2>&1 | FileCheck %s --check-prefix=NEGATIVE

   CHECK: C queue struct chan lifted
   OUT: recv -1
   NEGATIVE: built with a negative number of slots
*/
#include "chan.h"

struct chan q;

int main(int argc, char **argv) {
  int n = argc > 1 ? atoi(argv[1]) : 1;
  chan_open(&q, n, 1);
  chan_writer_done(&q);
  void *p;
  printf("recv %d\n", chan_recv(&q, &p));
  chan_close(&q);
  return 0;
}
