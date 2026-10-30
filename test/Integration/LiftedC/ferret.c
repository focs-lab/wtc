/* A model of the queue PARSEC's ferret keeps in C (apps/ferret/src/src/
   queue.c) and of its pipeline (benchmark/ferret-pthreads.c), written for
   this test: a ring of pointers under a pthread mutex with two condition
   variables, whose receive answers "finished" once it is empty and as many
   senders as the channel was opened for have said they are done. It uses
   ferret's forms: the full test as `put == (take - 1 + n) % n`, the index
   wrapped by `if (++i == n) i = 0`, the finished test after the wait. Three
   stages instead of six, a long per item instead of an image, threads
   started with pthread_create directly instead of through ferret's pool,
   the middle stage's thread count from argv as ferret's is.

   The channel lifts; the first has one sender and the middle stage's
   threads as receivers, the second those threads as senders and one
   receiver, so with a count from argv each is chosen when it is opened.
   With one middle thread fixed (ONE), both are proved spsc. The program
   runs with the contract checks and under ThreadSanitizer, with the
   output it has as written.

   POOL starts the stages as ferret does: through a pool that takes an
   array of descriptors, each naming its thread's entry, and starts one
   thread per descriptor in a loop over a count it is passed (ferret's
   tpool_create), the counts globals, the middle one from argv; and the
   loader reaches its queue through a recursive scan, as ferret's walks
   the query directory. The entries are read back from the descriptors,
   the recursion's push names the same queue at every depth, and each
   queue's tag is chosen when it is opened from the counts main passes to
   the pool: the same choice as with the threads started directly.

   RUN: %wtc_cc -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s
   RUN: %t.exe 1 | FileCheck %s --check-prefix=OUT
   RUN: %t.exe 4 | FileCheck %s --check-prefix=OUT
   RUN: %wtc_cc -O1 -DWTC_CHECK -DONE %s -o %t.one.exe 2>&1 | FileCheck %s --check-prefix=ONE
   RUN: %t.one.exe | FileCheck %s --check-prefix=OUT
   RUN: %wtc_cc -O1 --no-lift %s -o %t.orig.exe 2>&1 | FileCheck %s --check-prefix=ORIG --allow-empty
   RUN: %t.orig.exe 4 | FileCheck %s --check-prefix=OUT
   RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s %}
   RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 4 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
   RUN: %wtc_cc -O1 -DWTC_CHECK -DPOOL %s -o %t.pool.exe 2>&1 | FileCheck %s --check-prefix=POOL
   RUN: %t.pool.exe 1 | FileCheck %s --check-prefix=OUT
   RUN: %t.pool.exe 4 | FileCheck %s --check-prefix=OUT
   RUN: %if tsan %{ %wtc_cc --tsan -O1 -g -DPOOL %s -o %t.pool.tsan.exe 2>&1 | FileCheck %s --check-prefix=POOL %}
   RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.pool.tsan.exe 1 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}
   RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.pool.tsan.exe 4 2>&1 | FileCheck %s --check-prefixes=TSAN,OUT %}

   CHECK: C queue struct chan lifted (chan_open: init, chan_close: destroy, chan_writer_done: terminate, chan_recv: pop, chan_send: push); capacity: parameter 1 - 1; finished after as many terminations as parameter 2 of init
   CHECK: queue @to_work: spmc (producers: 1, consumers: many)
   CHECK: queue @to_out: mpsc (producers: many, consumers: 1)
   CHECK: specialised at run time: spsc when the loop at {{.*}} (consumers) runs at most once, else spmc
   CHECK: specialised at run time: spsc when the loop at {{.*}} (producers) runs at most once, else mpsc
   ONE: C queue struct chan lifted
   ONE: queue @to_work: spsc (producers: 1, consumers: 1)
   ONE: queue @to_out: spsc (producers: 1, consumers: 1)
   ORIG-NOT: lifted
   POOL-NOT: address of this function is taken
   POOL-NOT: unresolvable
   POOL: C queue struct chan lifted
   POOL: queue @to_work: mpmc (producers: many, consumers: many)
   POOL: queue @to_out: mpmc (producers: many, consumers: many)
   POOL: specialised at run time: spsc when the loops at {{.*}} (producers) and {{.*}} (consumers) each run at most once, mpsc when only the second does, spmc when only the first does, else generic
   POOL: specialised at run time: spsc when the loops at {{.*}} (producers) and {{.*}} (consumers) each run at most once, mpsc when only the second does, spmc when only the first does, else generic
   TSAN-NOT: ThreadSanitizer
   OUT: items 20000 sum 199990000
*/
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

struct chan {
  int put, take; /* the slot to fill next, the slot to empty next */
  void **slot;
  int nslots;
  int writers; /* how many senders say they are done before it is */
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
  if (++c->take == c->nslots) c->take = 0;
  pthread_cond_signal(&c->nonfull);
  pthread_mutex_unlock(&c->lock);
  return 0;
}

void chan_send(struct chan *c, void *v) {
  pthread_mutex_lock(&c->lock);
  while (c->put == (c->take - 1 + c->nslots) % c->nslots)
    pthread_cond_wait(&c->nonfull, &c->lock);
  c->slot[c->put] = v;
  if (++c->put == c->nslots) c->put = 0;
  pthread_cond_signal(&c->nonempty);
  pthread_mutex_unlock(&c->lock);
}

#define ITEMS 20000

struct chan to_work, to_out;
long total, count;

#ifdef POOL
/* ferret's loader reaches its queue through a recursive scan of the
   query directory: here, of a range, halved until it is short. */
static void send_one(long i) {
  long *item = (long *)malloc(sizeof(long));
  *item = i;
  chan_send(&to_work, item);
}

static void scan(long lo, long hi) {
  if (hi - lo <= 1000) {
    for (long i = lo; i < hi; ++i) send_one(i);
    return;
  }
  long mid = lo + (hi - lo) / 2;
  scan(lo, mid);
  scan(mid, hi);
}

void *loader(void *arg) {
  (void)arg;
  scan(0, ITEMS);
  chan_writer_done(&to_work);
  return NULL;
}
#else
void *loader(void *arg) {
  (void)arg;
  for (long i = 0; i < ITEMS; ++i) {
    long *item = (long *)malloc(sizeof(long));
    *item = i;
    chan_send(&to_work, item);
  }
  chan_writer_done(&to_work);
  return NULL;
}
#endif

void *worker(void *arg) {
  (void)arg;
  void *item;
  while (chan_recv(&to_work, &item) == 0) {
    *(long *)item *= 1;
    chan_send(&to_out, item);
  }
  chan_writer_done(&to_out);
  return NULL;
}

void *writer(void *arg) {
  (void)arg;
  void *item;
  while (chan_recv(&to_out, &item) == 0) {
    total += *(long *)item;
    count++;
    free(item);
  }
  return NULL;
}

#ifdef POOL
/* ferret's thread pool (src/tpool.c): an array of descriptors, one per
   thread, each naming the thread's entry and argument, which the pool
   starts in a loop over their number and gives up on at the first that
   fails to start. */
typedef void *(*entry_fn)(void *);
struct desc {
  const pthread_attr_t *attr;
  entry_fn start;
  void *arg;
};
struct pool {
  int n;
  pthread_t *ids;
};

struct pool *pool_start(struct desc *d, int n) {
  if (d == NULL || n < 1) return NULL;
  for (int i = 0; i < n; i++)
    if (d[i].start == NULL) return NULL;
  struct pool *p = (struct pool *)malloc(sizeof(struct pool));
  if (p == NULL) return NULL;
  p->ids = (pthread_t *)malloc(sizeof(pthread_t) * n);
  for (int i = 0; i < n; i++) {
    if (pthread_create(&p->ids[i], d[i].attr, d[i].start, d[i].arg) != 0) {
      free(p->ids);
      free(p);
      return NULL;
    }
  }
  p->n = n;
  return p;
}

void pool_join(struct pool *p) {
  for (int i = 0; i < p->n; i++) pthread_join(p->ids[i], NULL);
  free(p->ids);
  free(p);
}

/* The stages' sizes, globals as ferret's are; the middle one from argv. */
int n_load = 1, n_work = 1, n_out = 1;

int main(int argc, char **argv) {
  n_work = argc > 1 ? atoi(argv[1]) : 1;
  if (n_work < 1 || n_work > 16) n_work = 1;
  chan_open(&to_work, 20, n_load);
  chan_open(&to_out, 20, n_work);
  struct desc *dl = (struct desc *)calloc(n_load, sizeof(struct desc));
  struct desc *dw = (struct desc *)calloc(n_work, sizeof(struct desc));
  struct desc *dout = (struct desc *)calloc(n_out, sizeof(struct desc));
  dl[0].attr = NULL;
  dl[0].start = loader;
  dl[0].arg = NULL;
  dw[0].attr = NULL;
  dw[0].start = worker;
  dw[0].arg = NULL;
  for (int i = 1; i < n_work; i++) dw[i] = dw[0];
  dout[0].attr = NULL;
  dout[0].start = writer;
  dout[0].arg = NULL;
  struct pool *pl = pool_start(dl, n_load);
  struct pool *pw = pool_start(dw, n_work);
  struct pool *po = pool_start(dout, n_out);
  pool_join(pl);
  pool_join(pw);
  pool_join(po);
  free(dl);
  free(dw);
  free(dout);
  chan_close(&to_work);
  chan_close(&to_out);
  printf("items %ld sum %ld\n", count, total);
  return 0;
}
#else
int main(int argc, char **argv) {
#ifdef ONE
  const int n = 1;
  (void)argc, (void)argv;
#else
  int n = argc > 1 ? atoi(argv[1]) : 1;
  if (n < 1 || n > 16) n = 1;
#endif
  chan_open(&to_work, 20, 1);
  chan_open(&to_out, 20, n);
  pthread_t load, out, work[16];
  pthread_create(&load, NULL, loader, NULL);
#ifdef ONE
  pthread_create(&work[0], NULL, worker, NULL);
#else
  for (int i = 0; i < n; ++i) pthread_create(&work[i], NULL, worker, NULL);
#endif
  pthread_create(&out, NULL, writer, NULL);
  pthread_join(load, NULL);
  for (int i = 0; i < n; ++i) pthread_join(work[i], NULL);
  pthread_join(out, NULL);
  chan_close(&to_work);
  chan_close(&to_out);
  printf("items %ld sum %ld\n", count, total);
  return 0;
}
#endif
