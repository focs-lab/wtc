/* The critical sections of pthread_mutex_lock and pthread_mutex_unlock on
   one mutex (CriticalSections.h). An unlock in a branch that returns is an
   early exit, and the section goes on after the branch; the unlock in the
   function's own block ends it. A pointer parameter is the root of the
   member paths (arg0 here), a global the root of its own (@g).

   RUN: %clang -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-critical-sections -o /dev/null 2>&1 | FileCheck %s
*/
#include <pthread.h>

struct q {
  int head, tail, count;
  pthread_mutex_t lock;
};

// CHECK-DAG: remark: wtc: critical section in take on arg0.3, unlocked in its block; reads arg0.0 arg0.1; writes arg0.1; calls{{$}}
int take(struct q *p) {
  pthread_mutex_lock(&p->lock);
  if (p->head == p->tail) {
    pthread_mutex_unlock(&p->lock);
    return -1;
  }
  p->tail++;
  pthread_mutex_unlock(&p->lock);
  p->count = 0;
  return 0;
}

struct q g;
// CHECK-DAG: remark: wtc: critical section in bump on @g.3, unlocked in its block; reads @g.2; writes @g.2; calls{{$}}
void bump(void) {
  pthread_mutex_lock(&g.lock);
  g.count++;
  pthread_mutex_unlock(&g.lock);
}

int main(void) {
  pthread_mutex_init(&g.lock, 0);
  bump();
  return take(&g);
}
