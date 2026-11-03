/* The other file of embedded.c: it reads the queue's index through the
   global struct that holds it, as written. */
#include <pthread.h>
struct chan {
  int put, take;
  void **slot;
  int nslots;
  int writers;
  int done;
  pthread_mutex_t lock;
  pthread_cond_t nonempty, nonfull;
};
struct app {
  int id;
  struct chan q;
};
extern struct app A;
int peek(void) { return A.q.take; }
