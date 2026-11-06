/* A program whose channel is in another file (Inputs/unity_chan.c), as
   ferret's queue is in its library. wtc-cc compiles each source file to
   its CIR module and links them into one (wtc-link), so that the analysis
   sees both; --unity, the concatenation it replaced, is accepted and
   changes nothing. With the channel's file built apart and linked as an
   object, its functions are not in the module and nothing lifts; given as
   a source, the channel lifts, and with one thread on each side it is
   proved spsc.

   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/Inputs %s %S/Inputs/unity_chan.c -o %t.many.exe 2>&1 | FileCheck %s
   RUN: %t.many.exe | FileCheck %s --check-prefix=OUT
   RUN: %clang -O1 -c %S/Inputs/unity_chan.c -o %t.q.o
   RUN: %wtc_cc --lift -O1 -I%S/Inputs %s %t.q.o -o %t.apart.exe 2>&1 | FileCheck %s --check-prefix=APART --allow-empty
   RUN: %t.apart.exe | FileCheck %s --check-prefix=OUT
   RUN: %wtc_cc --lift --unity -O1 -DWTC_CHECK -I%S/Inputs %s %S/Inputs/unity_chan.c -o %t.exe 2>&1 | FileCheck %s
   RUN: %t.exe | FileCheck %s --check-prefix=OUT

   APART-NOT: C queue
   CHECK: C queue struct chan lifted
   CHECK: queue @jobs: spsc (producers: 1, consumers: 1)
   CHECK: specialised: constructor tag rewritten to spsc
   OUT: sum 499500
*/
#include "unity_chan.h"
#include <stdio.h>

struct chan jobs;
long sum;

void *producer(void *arg) {
  static long items[1000];
  (void)arg;
  for (long i = 0; i < 1000; ++i) {
    items[i] = i;
    chan_send(&jobs, &items[i]);
  }
  chan_writer_done(&jobs);
  return NULL;
}

void *consumer(void *arg) {
  void *x;
  (void)arg;
  while (chan_recv(&jobs, &x) == 0) sum += *(long *)x;
  return NULL;
}

int main(void) {
  pthread_t p, c;
  chan_open(&jobs, 8, 1);
  pthread_create(&p, NULL, producer, NULL);
  pthread_create(&c, NULL, consumer, NULL);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  chan_close(&jobs);
  printf("sum %ld\n", sum);
  return 0;
}
