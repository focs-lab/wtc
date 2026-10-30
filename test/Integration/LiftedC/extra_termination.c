/* A channel whose pop tests the end count with `>=` takes more
   terminations than producers as nothing new: the channel stays finished.
   The runtime used to end the program at the extra termination, as it
   still does for a channel that tests with `==` (ferret's), where the
   original goes wrong then (its pop takes from an empty ring).

   RUN: %wtc_cc -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.exe 2>&1 | FileCheck %s
   RUN: %t.exe | FileCheck %s --check-prefix=OUT
   RUN: %wtc_cc -O1 --no-lift -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.orig.exe
   RUN: %t.orig.exe | FileCheck %s --check-prefix=OUT

   CHECK: C queue struct chan lifted
   OUT-NOT: not finished
   OUT: sum 15 finished -1
*/
#define CUSTOM_RECV
#include "chan.h"

int chan_recv(struct chan *c, void **out) {
  pthread_mutex_lock(&c->lock);
  while (c->take == c->put && c->done < c->writers)
    pthread_cond_wait(&c->nonempty, &c->lock);
  if (c->take == c->put && c->done >= c->writers) {
    pthread_cond_broadcast(&c->nonempty);
    pthread_mutex_unlock(&c->lock);
    return -1;
  }
  *out = c->slot[c->take];
  if (++c->take == c->nslots) c->take = 0;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}

struct chan q;
void *producer(void *a) {
  for (long i = 1; i <= 5; ++i) chan_send(&q, (void *)i);
  chan_writer_done(&q);
  return a;
}
int main(void) {
  chan_open(&q, 4, 1);
  pthread_t t;
  pthread_create(&t, NULL, producer, NULL);
  void *p;
  long s = 0;
  while (chan_recv(&q, &p) == 0) s += (long)p;
  pthread_join(t, NULL);
  chan_writer_done(&q); /* one more: the channel is finished either way */
  int r = chan_recv(&q, &p);
  if (r != -1) printf("not finished\n");
  chan_close(&q);
  printf("sum %ld finished %d\n", s, r);
  return 0;
}
