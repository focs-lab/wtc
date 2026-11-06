/* Once an open finding (docs/lifting.md, "Findings of 1 Nov 2026"), the
   closed world: a queue inside a static struct, and a global another file
   names holds a pointer to a function that returns the struct's address.
   The search for the struct in the types of such globals does not enter
   function types, and a pointer to a struct that holds the queue may be
   returned. The other file reads the queue's index through the pointer.
   Lifted, it reads the substitute's pointer. Now the escape analysis
   follows the global to get_app, whose result the other file may then
   read, and the struct is refused; the program prints what it prints as
   written.

   RUN: %clang -O1 -c %S/Inputs/escape_function_pointer_b.c -o %t.b.o
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
   RUN: %t.exe | FileCheck %s --match-full-lines

   CHECK: take 2
   WHY: C queue struct chan not lifted: (a) a queue may be reached by code the pass does not see: get_app, whose address may reach code the pass does not see (the global g_get_app, which a file linked with this one names, may hold it), may be handed it or hand it back
*/
#include "chan.h"

struct app {
  int id;
  struct chan q;
};
static struct app A;
static struct app *get_app(void) { return &A; }
struct app *(*g_get_app)(void) = get_app;
int their_take(void); /* Inputs/escape_function_pointer_b.c */

int main(void) {
  chan_open(&A.q, 8, 1);
  for (long i = 1; i <= 3; ++i) chan_send(&A.q, (void *)i);
  void *p;
  chan_recv(&A.q, &p);
  chan_recv(&A.q, &p);
  printf("take %d\n", their_take());
  chan_writer_done(&A.q);
  chan_close(&A.q);
  return 0;
}
