/* The channel of unity.c (see there). */
#include "unity_chan.h"
#include <stdlib.h>

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
  while (c->take == c->put && c->done < c->writers) pthread_cond_wait(&c->nonempty, &c->lock);
  if (c->take == c->put) {
    pthread_mutex_unlock(&c->lock);
    return -1;
  }
  *out = c->slot[c->take];
  c->take = (c->take + 1) % c->nslots;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}

void chan_send(struct chan *c, void *v) {
  pthread_mutex_lock(&c->lock);
  while ((c->put + 1) % c->nslots == c->take) pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
  c->put = (c->put + 1) % c->nslots;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}
