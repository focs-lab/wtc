// Sections refused for the locks they take, from the review of 23 Nov
// 2026: a read lock of the named lock (readers that share it as written
// would run one at a time on the server, and one may wait for what
// another holds), and a section that lets go a lock its caller holds and
// takes it back (balanced in count, but the server would let go a lock
// another thread holds).
//
// RUN: %clang -std=gnu11 -O1 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=table -o /dev/null 2>&1 | FileCheck %s --check-prefix=READ
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s --check-prefix=DROP

#include <pthread.h>

pthread_rwlock_t table = PTHREAD_RWLOCK_INITIALIZER;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t aux = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t other = PTHREAD_RWLOCK_INITIALIZER;
static long hits, entries = 42, total;

// READ-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section read_and_count #1 on table not delegated: (c) takes the lock for reading: readers that share it as written would run one at a time on the server
void read_and_count(void) {
    pthread_rwlock_rdlock(&table);
    pthread_mutex_lock(&aux);
    hits++;
    pthread_mutex_unlock(&aux);
    pthread_rwlock_unlock(&table);
}

// The write sections of a read-write lock are exclusive, as the server is.
// READ-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section write #1 on table delegated
void write(long x) { pthread_rwlock_wrlock(&table); entries = x; pthread_rwlock_unlock(&table); }

// DROP-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section add_with_other_held #1 on lock not delegated: (c) lets go a lock it did not take within the section (pthread_rwlock_unlock): the server would let go a lock another thread holds
void add_with_other_held(long x) {
    pthread_mutex_lock(&lock);
    pthread_rwlock_unlock(&other);
    total += x;
    pthread_rwlock_wrlock(&other);
    pthread_mutex_unlock(&lock);
}

// Taken and let go in order: no obstacle.
// DROP-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section add_nested #1 on lock delegated
void add_nested(long x) { pthread_mutex_lock(&lock); pthread_mutex_lock(&aux); total += x; pthread_mutex_unlock(&aux); pthread_mutex_unlock(&lock); }

int main(void) { return 0; }
