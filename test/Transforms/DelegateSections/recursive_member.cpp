// A recursive pthread mutex as a class member with a default member
// initialiser (review of 23 Nov 2026): no global of type pthread_mutex_t
// and no mutex attribute, but a constant other than zeros.
//
// RUN: %clangxx -std=c++17 -O1 -fno-exceptions -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s

#include <pthread.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
struct Registry {
    pthread_mutex_t m = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
    long n = 0;
};
Registry reg;
static long total;

// CHECK: [[@LINE+2]]:{{.*}} remark: wtc: delegated section add #1 on lock not delegated: (c) takes a pthread or C11 mutex the pass cannot prove is not recursive: {{.*}}(a recursive initialiser)
void add(long x) {
    pthread_mutex_lock(&lock);
    pthread_mutex_lock(&reg.m);
    reg.n++;
    pthread_mutex_unlock(&reg.m);
    total += x;
    pthread_mutex_unlock(&lock);
}

int main() { return 0; }
