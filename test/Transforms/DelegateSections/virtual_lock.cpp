// A lock taken through a virtual call the pass resolves (review of 23 Nov
// 2026): the call's caller cannot count it, so a resolved target that
// takes or lets go a lock refuses the section.
//
// RUN: %clangxx -std=c++17 -O1 -fno-exceptions -fno-rtti -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s

#include <pthread.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t table = PTHREAD_RWLOCK_INITIALIZER;
static long total;

struct Step { virtual void run() = 0; };
struct TakeTable : Step { void run() override { pthread_rwlock_wrlock(&table); } };
struct Nothing : Step { void run() override {} };
struct Count : Step { void run() override { ++total; } };

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section add #1 on lock not delegated: (c) a call through a pointer may take or let go a lock (TakeTable::run)
void add(long x, Step *then) { pthread_mutex_lock(&lock); total += x; then->run(); pthread_mutex_unlock(&lock); }

struct Other { virtual void go() = 0; };
struct Inc : Other { void go() override { ++total; } };
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section other #1 on lock delegated
void other(Other *o) { pthread_mutex_lock(&lock); o->go(); pthread_mutex_unlock(&lock); }

int main() {
    Nothing n;
    TakeTable t;
    Count c;
    Inc i;
    add(1, &n);
    add(2, &t);
    add(3, &c);
    other(&i);
    return 0;
}
