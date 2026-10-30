/* A channel of the shape of ferret's queue (test/Integration/LiftedC/
   ferret.c), which lifts as written. A test replaces one of its functions
   by defining CUSTOM_<NAME> and writing its own, to show what that change
   makes the pass refuse. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct chan {
  int put, take;
  void **slot;
  int nslots;
  int writers;
  int done;
  pthread_mutex_t lock;
  pthread_cond_t nonempty, nonfull;
};

#ifndef CUSTOM_OPEN
void chan_open(struct chan *c, int nslots, int writers) {
  pthread_mutex_init(&c->lock, NULL);
  pthread_cond_init(&c->nonempty, NULL);
  pthread_cond_init(&c->nonfull, NULL);
  c->slot = (void **)malloc(sizeof(void *) * nslots);
  c->nslots = nslots;
  c->writers = writers;
  c->put = c->take = c->done = 0;
}
#endif

void chan_close(struct chan *c) {
  pthread_cond_destroy(&c->nonfull);
  pthread_cond_destroy(&c->nonempty);
  pthread_mutex_destroy(&c->lock);
  free(c->slot);
}

#ifndef CUSTOM_DONE
void chan_writer_done(struct chan *c) {
  pthread_mutex_lock(&c->lock);
  c->done++;
  pthread_cond_broadcast(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}
#endif

#ifndef CUSTOM_RECV
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
  if (++c->take == c->nslots) c->take = 0;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}
#endif

#ifndef CUSTOM_SEND
void chan_send(struct chan *c, void *v) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
    pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
  if (++c->put == c->nslots) c->put = 0;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}
#endif
