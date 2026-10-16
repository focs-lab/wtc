/* One struct per thread, allocated as an array and handed out by index:
   the shape of Phoenix's linear_regression and of memcached's threads[].
   Each worker bumps the head and the tail of its own element in a loop.
   At 40 bytes an element, the tail of one element and the head of the next
   share a cache line, and the two are owned by two different threads: that
   is false sharing between neighbours, and padding each element to a line
   removes it. Within one element there is one thread and no pair.

   RUN: %clang -std=gnu11 -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

   CHECK: remark: wtc: false sharing in calloc() in @main (one element per thread) across elements: 'tail' (offset 32) of one element and 'head' (offset 0) of the next
   CHECK-NOT: remark: wtc: possible false sharing
   CHECK: false sharing: {{[1-9][0-9]*}} definite, 0 possible, 0 cold
*/
#include <pthread.h>
#include <stdlib.h>

struct W { long head; long mid[3]; long tail; };

static void *worker(void *arg) {
    struct W *me = arg;
    for (long i = 0; i < 1000000; i++) { me->head++; me->tail++; }
    return 0;
}

int main(void) {
    int n = 4;
    struct W *w = calloc(n, sizeof *w);
    pthread_t t[4];
    for (int i = 0; i < n; i++) pthread_create(&t[i], 0, worker, &w[i]);
    for (int i = 0; i < n; i++) pthread_join(t[i], 0);
    return w[0].head == 0;
}
