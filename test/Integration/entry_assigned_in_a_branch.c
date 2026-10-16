/* A thread entry assigned to a function pointer inside an `if` and passed
   to pthread_create from there.

   The rule that stops a function from being taken for "callable from
   anywhere" when its address only flows into spawn entries followed the
   pointer through its slot under a looser notion of single assignment than
   the one that resolves the spawn. Here the first said "only a spawn
   entry", the second could not resolve the spawn, and count_misses ran on
   no thread at all: the pair was not reported. Both now use the same rule,
   so the spawn stays unresolved and the function stays a speculative root,
   which can only give `possible` -- the honest answer.

   RUN: %clang -std=gnu11 -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

   CHECK: possible false sharing in @stats
   CHECK: false sharing: 0 definite, 1 possible, 0 cold
*/

#include <pthread.h>

struct Stats { long hits; long misses; };
struct Stats stats;

static void *count_hits(void *arg) { for (int i = 0; i < 1000; ++i) ++stats.hits; return 0; }
static void *count_misses(void *arg) { for (int i = 0; i < 1000; ++i) ++stats.misses; return 0; }

int main(int argc, char **argv) {
    pthread_t t[2];
    void *(*f)(void *);
    pthread_create(&t[0], 0, count_hits, 0);
    if (argc > 0) {
        f = count_misses;
        pthread_create(&t[1], 0, f, 0);
    }
    pthread_join(t[0], 0);
    return 0;
}
