/* A local channel in main, nothing linked, at -O1: its lifetime markers
   are no use of the struct the confinement need refuse (a false refusal
   the review of 6 Nov 2026 found). It lifts, and prints what it prints as
   written.

   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=LIFT
   RUN: %t.exe | FileCheck %s

   LIFT: C queue struct chan lifted (
   CHECK: got 1
*/
#include "chan.h"
int main(void) {
  struct chan c;
  chan_open(&c, 8, 1);
  for (long i = 1; i <= 3; ++i) chan_send(&c, (void *)i);
  void *p;
  chan_recv(&c, &p);
  printf("got %ld\n", (long)p);
  chan_writer_done(&c);
  chan_close(&c);
  return 0;
}
