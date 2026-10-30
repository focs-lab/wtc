/* (a) A queue inside another struct (`struct app { int id; struct chan q; }`)
   is reached by whatever reaches that struct. Its objects are confined as
   the queue's own are: refused when another file names a global that holds
   one (NAMED, as nm lists `A` for a file that reads A.q.take), when one is
   initialised statically (STATIC, where init never builds the queue inside
   it, and the lifted program crashed), copied whole (COPY), or handed to a
   function with no body here (PASSED) or as another type (VOIDPTR). Used
   only through pointers to its queue, it lifts, also when a linked file
   names functions over a list of its own that points to itself (LIST:
   one defined here, one only declared), which reach no queue.

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -o %t.plain.cir
   RUN: wtc-opt %t.plain.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PLAIN
   RUN: echo A > %t.names
   RUN: wtc-opt %t.plain.cir --wtc-lift-c-queues="rewrite=false named-elsewhere=%t.names" -o /dev/null 2>&1 | FileCheck %s --check-prefix=NAMED
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DSTATIC -o %t.static.cir
   RUN: wtc-opt %t.static.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=STATIC
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DCOPY -o %t.copy.cir
   RUN: wtc-opt %t.copy.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=COPY
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DPASSED -o %t.passed.cir
   RUN: wtc-opt %t.passed.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PASSED
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DVOIDPTR -o %t.void.cir
   RUN: wtc-opt %t.void.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=VOIDPTR
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DLIST -o %t.list.cir
   RUN: printf 'walk\nlength\n' > %t.list
   RUN: wtc-opt %t.list.cir --wtc-lift-c-queues="rewrite=false named-elsewhere=%t.list" -o /dev/null 2>&1 | FileCheck %s --check-prefix=PLAIN

   PLAIN: C queue struct chan lifts
   NAMED: C queue struct chan not lifted: (a) A is named by another file linked with this one
   STATIC: C queue struct chan not lifted: (a) A is initialised statically, not by chan_open
   COPY: C queue struct chan not lifted: (a) main copies a struct that holds the queue
   PASSED: C queue struct chan not lifted: (a) main passes a pointer to a struct that holds the queue to inspect, which has no body here
   VOIDPTR: C queue struct chan not lifted: (a) main converts a pointer to a struct that holds the queue to another type
*/
#include "chan.h"
#include <string.h>

struct app {
  int id;
  struct chan q;
};
#if defined(STATIC)
static void *buf[4];
struct app A = {1, {0, 0, buf, 4, 1, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER}};
#else
struct app A;
#endif
#if defined(LIST)
struct node {
  struct node *next;
  long v;
};
int length(struct node *n); /* in another file */
long walk(struct node *n) { return n ? n->v + walk(n->next) : 0; }
#endif
#if defined(COPY)
struct app B;
#elif defined(PASSED)
void inspect(struct app *a); /* in another file */
#endif

int main(void) {
#if !defined(STATIC)
  chan_open(&A.q, 8, 1);
#endif
  for (long i = 1; i <= 3; ++i) chan_send(&A.q, (void *)i);
  void *p;
  chan_recv(&A.q, &p);
#if defined(LIST)
  struct node tail = {0, 2}, head = {&tail, 1};
  printf("%ld %d\n", walk(&head), length(&head));
#endif
#if defined(COPY)
  B = A;
#elif defined(PASSED)
  inspect(&A);
#elif defined(VOIDPTR)
  memset(&A, 0, sizeof A.id);
#endif
  chan_writer_done(&A.q);
  chan_close(&A.q);
  return 0;
}
