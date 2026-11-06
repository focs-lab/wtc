/* Once an open finding (docs/lifting.md, "Findings of 1 Nov 2026"), the
   closed world: the queue's storage belongs to another file. A function
   with no body here returns a pointer to it, and its uses here are only
   the queue's functions; the other file reads the queue's index as
   written. EMBED: the same through a struct that holds the queue. Lifted,
   the other file reads the substitute's pointer. Now the escape analysis
   takes the result of a call with no body here for the other file's, and
   the struct is refused; the program prints what it prints as written.

   RUN: %clang -O1 -c %S/Inputs/escape_storage_from_call_b.c -o %t.b.o
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
   RUN: %t.exe | FileCheck %s --match-full-lines
   RUN: %clang -O1 -DEMBED -c %S/Inputs/escape_storage_from_call_b.c -o %t.embed.b.o
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DEMBED -I%S/../../Transforms/LiftCQueues/Inputs %s %t.embed.b.o -o %t.embed.exe 2>&1 | FileCheck %s --check-prefix=WHY
   RUN: %t.embed.exe | FileCheck %s --match-full-lines

   CHECK: take 2
   WHY: C queue struct chan not lifted: (a) a queue may be reached by code the pass does not see: main takes a value that may point to it from their_{{chan|app}}, which has no body here
*/
#include "chan.h"

#ifdef EMBED
struct app {
  int id;
  struct chan q;
};
struct app *their_app(void); /* Inputs/escape_storage_from_call_b.c */
#define Q (&their_app()->q)
#else
struct chan *their_chan(void); /* Inputs/escape_storage_from_call_b.c */
#define Q (their_chan())
#endif
int their_take(void);

int main(void) {
  chan_open(Q, 8, 1);
  for (long i = 1; i <= 3; ++i) chan_send(Q, (void *)i);
  void *p;
  chan_recv(Q, &p);
  chan_recv(Q, &p);
  printf("take %d\n", their_take());
  chan_writer_done(Q);
  chan_close(Q);
  return 0;
}
