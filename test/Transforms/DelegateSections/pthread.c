// A C program's global pthread mutex, taken directly and through lock
// helpers of its own, as C programs write them. A second run sets a mutex
// attribute's type somewhere in the module: any pthread mutex may then be
// recursive, and nothing is delegated.
//
// RUN: %clang -std=gnu11 -O1 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=lock -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir
// RUN: %clang -std=gnu11 -O1 -DRECURSIVE -fclangir -emit-cir %s -o %t.rec.cir
// RUN: wtc-opt %t.rec.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s --check-prefix=REC

#include <pthread.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long total, hits;

static void take(void) { pthread_mutex_lock(&lock); }
static void give(void) { pthread_mutex_unlock(&lock); }

// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section add #1 on lock delegated
long add(long x) {
    pthread_mutex_lock(&lock);
    total += x;
    long t = total;
    pthread_mutex_unlock(&lock);
    return t;
}

// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section hit #1 on lock delegated
void hit(void) {
    take();
    ++hits;
    give();
}

// The whole program: no file the pass does not see may make it recursive.
int main(void) { return 0; }

#ifdef RECURSIVE
void make_recursive(pthread_mutex_t *m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
}
#endif

// REC-DAG: remark: wtc: delegated section add #1 on lock not delegated: (c) takes a pthread or C11 mutex the pass cannot prove is not recursive: the module sets a mutex attribute's type (pthread_mutexattr_settype)
// REC-DAG: remark: wtc: delegated section hit #1 on lock not delegated: (c) takes a pthread or C11 mutex the pass cannot prove is not recursive: the module sets a mutex attribute's type

// IR: cir.func {{.*}}@add(
// IR: wtc.delegate %{{.*}}, @add.wtc.section.{{[0-9]+}}(%{{.*}}) {mutex_name = "lock"}
// IR: cir.func {{.*}}@hit(
// IR: wtc.delegate %{{.*}}, @hit.wtc.section.{{[0-9]+}}(%{{.*}}) {mutex_name = "lock"}
