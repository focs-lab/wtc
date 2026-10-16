/* Two global constructors: the first starts a worker, the second counts in
   a field next to the worker's. Nothing guarantees the second runs before
   the worker does, so its loop is not start-up.

   When a constructor's spawn was first made to end the start-up phase,
   only main lost the start-up flag; every constructor kept it, so the
   second one's loop was taken for start-up and the pair came out cold --
   "costs nothing" -- which is false. When any constructor starts a
   thread, no constructor is start-up now.

   RUN: %clang -std=gnu11 -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

   CHECK: remark: wtc: false sharing in @stats
   CHECK: false sharing: 1 definite, 0 possible, 0 cold
*/

#include <pthread.h>

struct Stats { long hits; long misses; };
struct Stats stats;
static pthread_t t;

static void *count_misses(void *arg) { for (int i = 0; i < 1000; ++i) ++stats.misses; return 0; }

__attribute__((constructor)) static void start(void) { pthread_create(&t, 0, count_misses, 0); }
__attribute__((constructor)) static void later(void) { for (int i = 0; i < 1000; ++i) ++stats.hits; }

int main(void) { return 0; }
