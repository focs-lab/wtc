/* Known open finding (docs/lifting.md, "Known open findings"): ferret's
   channel with its indices kept in unsigned chars. With 300 slots the
   original's put wraps to 0 after 255 (++ on an unsigned char, or REM: the
   int result of (put + 1) % nslots converted back), so the 257th and
   258th sends overwrite slots 0 and 1, and the receiver gets two elements.
   The C ring's evaluator takes the increment, and an integral conversion
   that loses values, for the identity: lifted, the substitute is a ring of
   299 places and hands back all 258. The test passes once the struct is
   refused here, or lifted keeps what the program prints as written.

   XFAIL: *
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe
   RUN: %t.exe | FileCheck %s --match-full-lines
   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DREM %s -o %t.rem.exe
   RUN: %t.rem.exe | FileCheck %s --match-full-lines

   CHECK: received 2 sum 515
*/
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
struct chan {
  unsigned char put, take;
  void **slot;
  int nslots;
  int writers;
  int done;
  pthread_mutex_t lock;
  pthread_cond_t nonempty, nonfull;
};
void chan_open(struct chan *c, int nslots, int writers) {
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->nonempty, NULL);
  pthread_cond_init(&c->nonfull, NULL);
  c->slot = (void **)malloc(sizeof(void *) * nslots);
  c->nslots = nslots;
  c->writers = writers;
  c->put = c->take = c->done = 0;
}
void chan_close(struct chan *c) {
  pthread_cond_destroy(&c->nonfull);
  pthread_cond_destroy(&c->nonempty);
  pthread_mutex_destroy(&c->lock);
  free(c->slot);
}
void chan_writer_done(struct chan *c) {
  pthread_mutex_lock(&c->lock);
  c->done++;
  pthread_cond_broadcast(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}
int chan_recv(struct chan *c, void **out) {
  pthread_mutex_lock(&c->lock);
  while (c->take == c->put && c->done < c->writers)
    pthread_cond_wait(&c->nonempty, &c->lock);
  if (c->take == c->put && c->done == c->writers) {
    pthread_cond_broadcast(&c->nonempty);
    pthread_mutex_unlock(&c->lock);
    return -1;
  }
  *out = c->slot[c->take];
#ifdef REM
  c->take = (c->take + 1) % c->nslots;
#else
  if (++c->take == c->nslots) c->take = 0;
#endif
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}
void chan_send(struct chan *c, void *v) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
    pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
#ifdef REM
  c->put = (c->put + 1) % c->nslots;
#else
  if (++c->put == c->nslots) c->put = 0;
#endif
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}

struct chan q;
int main(void) {
  chan_open(&q, 300, 1);
  for (long i = 1; i <= 258; ++i) chan_send(&q, (void *)i);
  chan_writer_done(&q);
  void *p;
  long n = 0, s = 0;
  while (chan_recv(&q, &p) == 0) { ++n; s += (long)p; }
  printf("received %ld sum %ld\n", n, s);
  chan_close(&q);
  return 0;
}
