// Sections refused for their shape (docs/delegation.md, "The legality
// proof"), each in a C function: a lock of another mutex with a time
// limit (d); a goto, a section that never lets the mutex go, and one
// entered on a path that does not take the lock, or by a loop that comes
// back to its lock (b). A second run marks the module open-world: any pthread
// mutex may then be made recursive by a file the pass does not see, and no
// section is delegated, the one that passes here included.
//
// RUN: %clang -std=gnu11 -O1 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s
// RUN: wtc-opt %t.cir --wtc-delegate-sections="mutex=lock open-world=true" -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPEN

#include <pthread.h>
#include <time.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
static long total;
volatile int flag;

// CHECK-DAG: [[@LINE+4]]:{{.*}} remark: wtc: delegated section timed #1 on lock not delegated: (d) waits for a lock with a time limit (pthread_mutex_timedlock)
// OPEN-DAG: remark: wtc: delegated section timed #1 on lock not delegated: (c) takes a pthread or C11 mutex the pass cannot prove is not recursive: files the pass does not see may make a pthread or C11 mutex recursive
long timed(void) {
    struct timespec ts = {0, 0};
    pthread_mutex_lock(&lock);
    long t = 0;
    if (pthread_mutex_timedlock(&other, &ts) == 0) {
        t = ++total;
        pthread_mutex_unlock(&other);
    }
    pthread_mutex_unlock(&lock);
    return t;
}

// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section jumps #1 on lock not delegated: (b) jumps by goto
long jumps(long x) {
    pthread_mutex_lock(&lock);
    if (x < 0) goto out;
    total += x;
out:
    pthread_mutex_unlock(&lock);
    return x;
}

// The first section ends in a trap: no path lets the mutex go.
// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: delegated section stuck #1 on lock not delegated: (b) never lets the mutex go in stuck
void stuck(int c) {
    if (c) {
        pthread_mutex_lock(&lock);
        total++;
        __builtin_trap();
    }
    pthread_mutex_lock(&lock);
    total--;
    pthread_mutex_unlock(&lock);
}

// The increment runs with the mutex held on one path only.
// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section maybe #1 on lock not delegated: (b) is entered other than through its lock
void maybe(int c) {
    if (c) pthread_mutex_lock(&lock);
    total++;
    pthread_mutex_unlock(&lock);
}

// The continue goes back to the lock with the mutex held: the loop's
// head, in the section, is also entered from before the loop. (The trap
// after the loop keeps the loop's exit, which the front end emits, from
// reaching the return, which would refuse it first.)
// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: delegated section spin #1 on lock not delegated: (b) is entered other than through its lock
void spin(void) {
    while (1) {
        pthread_mutex_lock(&lock);
        total++;
        if (flag) continue;
        pthread_mutex_unlock(&lock);
    }
    __builtin_trap();
}

// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section add #1 on lock delegated
long add(long x) {
    pthread_mutex_lock(&lock);
    total += x;
    long t = total;
    pthread_mutex_unlock(&lock);
    return t;
}
// OPEN-DAG: remark: wtc: delegated section add #1 on lock not delegated: (c) takes a pthread or C11 mutex the pass cannot prove is not recursive: files the pass does not see may make a pthread or C11 mutex recursive

int main(void) { return 0; }
