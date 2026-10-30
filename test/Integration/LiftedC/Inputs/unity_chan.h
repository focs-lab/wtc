/* The channel of unity.c, in a file of its own, as PARSEC keeps ferret's
   queue in its library. */
#ifndef UNITY_CHAN_H
#define UNITY_CHAN_H
#include <pthread.h>

struct chan {
  int put, take;
  void **slot;
  int nslots, writers, done;
  pthread_mutex_t lock;
  pthread_cond_t nonempty, nonfull;
};

void chan_open(struct chan *c, int nslots, int writers);
void chan_close(struct chan *c);
void chan_writer_done(struct chan *c);
int chan_recv(struct chan *c, void **out);
void chan_send(struct chan *c, void *v);
#endif
