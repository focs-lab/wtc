/* The other file of open_storage_from_call.c, built as written. */
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
#ifdef EMBED
struct app {
  int id;
  struct chan q;
};
static struct app theirs;
struct app *their_app(void) { return &theirs; }
int their_take(void) { return theirs.q.take; }
#else
static struct chan theirs;
struct chan *their_chan(void) { return &theirs; }
int their_take(void) { return theirs.take; }
#endif
