/* The same two counters, started with pthread_create rather than
   std::thread. Recognising this matters for more than tidiness: without it
   the analysis knows a program has threads but not where they begin, so the
   start-up phase cannot be established, and every field written while
   parsing options pairs against every worker that later reads the struct.
   Amalgamated memcached showed exactly that.

   RUN: %clang -std=gnu11 -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

   CHECK: remark: wtc: false sharing in @stats
   CHECK-SAME: 'hits'
   CHECK-SAME: 'misses'
   CHECK: false sharing: 1 definite, 0 possible, 0 cold
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

int main(void) {
    pthread_t a, b;
    pthread_create(&a, 0, count_hits, 0);
    pthread_create(&b, 0, count_misses, 0);
    pthread_join(a, 0);
    pthread_join(b, 0);
    return (int)(stats.hits + stats.misses);
}
