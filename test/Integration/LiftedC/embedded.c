/* A queue inside another global struct. Another file reads the queue's
   index through that struct (NAMED); lifted, it read the substitute's
   pointer. Another queue inside a global struct is initialised statically
   (STATIC), never built by init; lifted, its operations found no queue and
   the program crashed. Both are refused, and the programs run as written.

   RUN: %clang -O1 -c %S/Inputs/embedded_b.c -o %t.b.o
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DNAMED -I%S/../../Transforms/LiftCQueues/Inputs %s %t.b.o -o %t.named.exe 2>&1 | FileCheck %s --check-prefix=NAMED
   RUN: %t.named.exe | FileCheck %s --check-prefix=NAMED-OUT
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DSTATIC -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.static.exe 2>&1 | FileCheck %s --check-prefix=STATIC
   RUN: %t.static.exe | FileCheck %s --check-prefix=STATIC-OUT

   NAMED: C queue struct chan not lifted: (a) a queue may be reached by code the pass does not see: the global A, which a file linked with this one names, may hold it
   NAMED-OUT: take 2
   STATIC: C queue struct chan not lifted: (a) A is initialised statically, not by chan_open
   STATIC-OUT: sum 15 fixed 150
*/
#include "chan.h"

#if defined(NAMED)
struct app {
  int id;
  struct chan q;
} A;
int peek(void); /* Inputs/embedded_b.c */

int main(void) {
  chan_open(&A.q, 8, 1);
  for (long i = 1; i <= 3; ++i) chan_send(&A.q, (void *)i);
  void *p;
  chan_recv(&A.q, &p);
  chan_recv(&A.q, &p);
  printf("take %d\n", peek());
  chan_writer_done(&A.q);
  chan_close(&A.q);
  return 0;
}
#else
static void *buf[4];
struct app {
  int id;
  struct chan fixed;
} A = {1, {0, 0, buf, 4, 1, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER}};
struct chan q;

static void *producer(void *a) {
  for (long i = 1; i <= 5; ++i) {
    chan_send(&q, (void *)i);
    chan_send(&A.fixed, (void *)(10 * i));
  }
  chan_writer_done(&q);
  chan_writer_done(&A.fixed);
  return a;
}

int main(void) {
  chan_open(&q, 4, 1);
  pthread_t t;
  pthread_create(&t, NULL, producer, NULL);
  void *p;
  long s = 0, f = 0;
  int a = 0, b = 0;
  while (!(a && b)) {
    if (!a) {
      if (chan_recv(&q, &p) == 0) s += (long)p;
      else a = 1;
    }
    if (!b) {
      if (chan_recv(&A.fixed, &p) == 0) f += (long)p;
      else b = 1;
    }
  }
  pthread_join(t, NULL);
  printf("sum %ld fixed %ld\n", s, f);
  return 0;
}
#endif
