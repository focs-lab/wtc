/* Known open finding (docs/lifting.md, "Known open findings"), the closed
   world: the queue's storage belongs to another file. A function with no
   body here returns a pointer to it, and its uses here are only the
   queue's functions; the other file reads the queue's index as written.
   EMBED: the same through a struct that holds the queue. Lifted, the other
   file reads the substitute's pointer. The test passes once the struct is
   refused here, or lifted keeps what the program prints as written.

   XFAIL: *
   RUN: %clang -O1 -c %S/Inputs/open_storage_from_call_b.c -o %t.b.o
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s %t.b.o -o %t.exe
   RUN: %t.exe | FileCheck %s --match-full-lines
   RUN: %clang -O1 -DEMBED -c %S/Inputs/open_storage_from_call_b.c -o %t.embed.b.o
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DEMBED -I%S/../../Transforms/LiftCQueues/Inputs %s %t.embed.b.o -o %t.embed.exe
   RUN: %t.embed.exe | FileCheck %s --match-full-lines

   CHECK: take 2
*/
#include "chan.h"

#ifdef EMBED
struct app {
  int id;
  struct chan q;
};
struct app *their_app(void); /* Inputs/open_storage_from_call_b.c */
#define Q (&their_app()->q)
#else
struct chan *their_chan(void); /* Inputs/open_storage_from_call_b.c */
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
