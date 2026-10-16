/* Whether two fields share a cache line depends on where the object
   starts, and unless it is aligned to a line the linker decides that.

   `a` and `b` are 56 bytes apart: they share a line only when `s` starts
   on a boundary. `b` and `c` are adjacent across offset 64: they share a
   line unless it does. An earlier version assumed every object starts on
   a boundary, so it reported the first pair as if certain and never
   reported the second; a build of this program that prints `&s % 64`
   found `s` 16 bytes past a boundary, the reverse of what was assumed.
   Each pair is now reported with the share of the positions it shares a
   line in.

   RUN: %clang -std=gnu11 -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-false-sharing-diagnose 2>&1 >/dev/null | FileCheck %s

   CHECK-DAG: remark: wtc: false sharing in @s: 'a' (offset 0) and 'b' (offset 56) {{.*}} in 1 of the 8 positions an object aligned to 8 bytes may start at
   CHECK-DAG: remark: wtc: false sharing in @s: 'b' (offset 56) and 'c' (offset 64) {{.*}} in 7 of the 8 positions an object aligned to 8 bytes may start at
*/

#include <pthread.h>

struct S { long a; long pad[6]; long b; long c; };
char before[24] = {1};
struct S s;

static void *wb(void *p) { for (int i = 0; i < 1000; ++i) s.b = s.b + 1; return 0; }
static void *wc(void *p) { for (int i = 0; i < 1000; ++i) s.c = s.c + 1; return 0; }

int main(void) {
    pthread_t t, u;
    pthread_create(&t, 0, wb, 0);
    pthread_create(&u, 0, wc, 0);
    for (int i = 0; i < 1000; ++i) s.a = s.a + 1;
    pthread_join(t, 0);
    pthread_join(u, 0);
    return 0;
}
