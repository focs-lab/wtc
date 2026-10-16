/* Workers started the way memcached starts them: a helper takes the entry
   function as a parameter and passes it to pthread_create. memcached's
   create_worker(worker_libevent, &threads[i]) is this shape, called in a
   loop for the pool.

   The walk resolved a spawn's entry only when it named a function
   directly, so here it found pthread_create's entry to be a parameter,
   gave up on it, and treated count_hits and count_misses as functions
   whose address is merely taken -- callable from anywhere, by threads it
   cannot account for. Every pair memcached's workers take part in was
   invisible as a result. The entry is now resolved through the call site
   that passed it, like any other parameter.

   RUN: %clang -std=gnu11 -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

   CHECK-NOT: address of this function is taken
   CHECK-NOT: spawn target could not be resolved
   CHECK: remark: wtc: false sharing in @stats
   CHECK-SAME: 'hits'
   CHECK-SAME: 'misses'
   CHECK: false sharing: 1 definite, 0 possible
*/

#include <pthread.h>

struct Stats { long hits; long misses; };
struct Stats stats;

static void *count_hits(void *arg) {
    for (int i = 0; i < 1000; ++i) ++stats.hits;
    return 0;
}
static void *count_misses(void *arg) {
    for (int i = 0; i < 1000; ++i) ++stats.misses;
    return 0;
}

static pthread_t tids[5];
static int ntids;

static void create_worker(void *(*func)(void *), void *arg) {
    pthread_create(&tids[ntids++], 0, func, arg);
}

int main(void) {
    for (int i = 0; i < 4; ++i)
        create_worker(count_hits, 0);
    create_worker(count_misses, 0);
    for (int i = 0; i < ntids; ++i)
        pthread_join(tids[i], 0);
    return (int)(stats.hits + stats.misses);
}
