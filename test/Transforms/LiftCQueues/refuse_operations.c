/* What the operations must each do, one variant of the channel
   (Inputs/chan.h) per refusal: an initialisation on one path that
   allocates the ring (a); a push that holds the mutex from its first touch
   of the queue to its last (b); a pop that, after its wait, takes exactly
   when the ring is not empty and answers "finished" exactly when it is and
   every writer is done (d).

   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DOPEN_PATHS -o %t.paths.cir
   RUN: wtc-opt %t.paths.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=PATHS
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DOPEN_STATIC -o %t.static.cir
   RUN: wtc-opt %t.static.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=STATIC
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DSEND_RELOCK -o %t.relock.cir
   RUN: wtc-opt %t.relock.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=RELOCK
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DRECV_EMPTY -o %t.empty.cir
   RUN: wtc-opt %t.empty.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=EMPTY
   RUN: %clang -fclangir -emit-cir -I%S/Inputs %s -DRECV_GIVEUP -o %t.giveup.cir
   RUN: wtc-opt %t.giveup.cir --wtc-lift-c-queues=rewrite=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=GIVEUP

   PATHS: C queue struct chan not lifted: (a) chan_open initialises the queue on several paths
   STATIC: C queue struct chan not lifted: (e) chan_open allocates no ring
   RELOCK: C queue struct chan not lifted: (b) chan_send lets the mutex go in the middle
   EMPTY: C queue struct chan not lifted: (d) chan_recv takes from an empty ring
   GIVEUP: C queue struct chan not lifted: (d) chan_recv gives up while elements are queued
*/
#if defined(OPEN_PATHS) || defined(OPEN_STATIC)
#define CUSTOM_OPEN
#endif
#if defined(SEND_RELOCK)
#define CUSTOM_SEND
#endif
#if defined(RECV_EMPTY) || defined(RECV_GIVEUP)
#define CUSTOM_RECV
#endif
#include "chan.h"

/* Two paths: the slot count raised to two first. */
#if defined(OPEN_PATHS)
void chan_open(struct chan *c, int nslots, int writers) {
  if (nslots < 2) nslots = 2;
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->nonempty, NULL);
  pthread_cond_init(&c->nonfull, NULL);
  c->slot = (void **)malloc(sizeof(void *) * nslots);
  c->nslots = nslots;
  c->writers = writers;
  c->put = c->take = c->done = 0;
}
#elif defined(OPEN_STATIC)
/* The ring is a static array, not an allocation. */
static void *storage[8];
void chan_open(struct chan *c, int nslots, int writers) {
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->nonempty, NULL);
  pthread_cond_init(&c->nonfull, NULL);
  c->slot = storage;
  c->nslots = nslots;
  c->writers = writers;
  c->put = c->take = c->done = 0;
}
#endif

/* The mutex let go after the wait and taken again to write. */
#if defined(SEND_RELOCK)
void chan_send(struct chan *c, void *v) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
    pthread_cond_wait(&c->nonfull, &c->lock);
  pthread_mutex_unlock(&c->lock);
  pthread_mutex_lock(&c->lock);
  c->slot[c->put] = v;
  if (++c->put == c->nslots) c->put = 0;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}
#endif

/* Takes after the wait whether or not the ring is empty. */
#if defined(RECV_EMPTY)
int chan_recv(struct chan *c, void **out) {
  pthread_mutex_lock(&c->lock);
  while (c->take == c->put && c->done < c->writers)
    pthread_cond_wait(&c->nonempty, &c->lock);
  *out = c->slot[c->take];
  if (++c->take == c->nslots) c->take = 0;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}
#elif defined(RECV_GIVEUP)
/* Answers "finished" once every writer is done, elements queued or not. */
int chan_recv(struct chan *c, void **out) {
  pthread_mutex_lock(&c->lock);
  while (c->take == c->put && c->done < c->writers)
    pthread_cond_wait(&c->nonempty, &c->lock);
  if (c->done == c->writers) {
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
#endif

struct chan q;
int main(void) {
  chan_open(&q, 8, 1);
  chan_send(&q, (void *)1);
  chan_writer_done(&q);
  void *p;
  while (chan_recv(&q, &p) == 0)
    ;
  chan_close(&q);
  return 0;
}
