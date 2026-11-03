/* The other file of open_function_pointer.c, built as written. */
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
extern struct app *(*g_get_app)(void);
int their_take(void) { return g_get_app()->q.take; }
