/* A program whose channel is in another file (Inputs/unity_chan.c), as
   ferret's queue is in its library: wtc-cc analyses one translation unit,
   and --unity makes one of several files, unedited, by including each of
   them in a generated file. Without it, wtc-cc says that it takes one
   unit; with the channel's file built apart and linked, or with --unity,
   the program is built from C as written and computes what it did.

   RUN: not %wtc_cc %s %S/Inputs/unity_chan.c -o %t.exe 2>&1 | FileCheck %s --check-prefix=MANY
   RUN: %clang -O1 -c %S/Inputs/unity_chan.c -o %t.q.o
   RUN: %wtc_cc -O1 -I%S/Inputs %s %t.q.o -o %t.apart.exe 2>&1 | FileCheck %s --check-prefix=APART --allow-empty
   RUN: %t.apart.exe | FileCheck %s --check-prefix=OUT
   RUN: %wtc_cc --unity -O1 -DWTC_CHECK -I%S/Inputs %s %S/Inputs/unity_chan.c -o %t.exe 2>&1 | FileCheck %s --allow-empty
   RUN: %t.exe | FileCheck %s --check-prefix=OUT

   MANY: wtc-cc: 2 source files: the analysis takes one translation unit; pass --unity to compile them as one
   APART-NOT: error
   CHECK-NOT: error
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
