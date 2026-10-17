/* A pointer field set in two places, as memcached sets `c->thread` where
   a connection is made: the worker writes through it in a loop, and the
   analysis, listing every store into the field, places the write on both
   objects as one of the two. Against the thread that bumps the
   neighbouring field of each object that is a possible pair on each,
   never a definite one -- the pointer names only one of them.

   RUN: %clang -std=gnu11 -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

   CHECK-NOT: remark: wtc: false sharing in
   CHECK: remark: wtc: possible false sharing in @ta: 'a' and 'b' share one 64-byte line in 7 of the 8 positions an object aligned to 8 bytes may start at and are touched repeatedly, but through a pointer that may name one of several objects, this among them
   CHECK: remark: wtc: possible false sharing in @tb: 'a' and 'b' share one 64-byte line in 7 of the 8 positions an object aligned to 8 bytes may start at and are touched repeatedly, but through a pointer that may name one of several objects, this among them
   CHECK: false sharing: 0 definite, 2 possible, 0 cold{{.*}}, 0 field accesses on objects that could not be identified{{.*}}; 2 accesses placed as one of several objects
*/
#include <pthread.h>

struct T { long a; long b; };
struct C { struct T *t; };

static struct T ta, tb;
static struct C c;

static void bind_a(void) { c.t = &ta; }
static void bind_b(void) { c.t = &tb; }

static void *worker(void *arg) {
    for (long i = 0; i < 1000000; i++) c.t->a++;
    return 0;
}

static void *other(void *arg) {
    for (long i = 0; i < 1000000; i++) { ta.b++; tb.b++; }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1) bind_a(); else bind_b();
    pthread_t w, o;
    pthread_create(&w, 0, worker, 0);
    pthread_create(&o, 0, other, 0);
    pthread_join(w, 0);
    pthread_join(o, 0);
    return 0;
}
