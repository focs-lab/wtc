// A pthread or C11 mutex taken inside a section must be proved not to be
// recursive: the server would wait for one its client holds, where the
// client would have taken it again (review of 23 Nov 2026). Each run has
// one way the module may make one recursive: a recursive initialiser in a
// member of a global struct, mtx_init with mtx_recursive, and, without
// main, files the pass does not see.
//
// RUN: %clang -std=gnu11 -O1 -DSTRUCT -fclangir -emit-cir %s -o %t.s.cir
// RUN: wtc-opt %t.s.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s --check-prefix=STRUCT
// RUN: %clang -std=gnu11 -O1 -DC11 -fclangir -emit-cir %s -o %t.c.cir
// RUN: wtc-opt %t.c.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s --check-prefix=C11
// RUN: %clang -std=gnu11 -O1 -DNOMAIN -fclangir -emit-cir %s -o %t.n.cir
// RUN: wtc-opt %t.n.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOMAIN
// RUN: %clang -std=gnu11 -O1 -fclangir -emit-cir %s -o %t.p.cir
// RUN: wtc-opt %t.p.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s --check-prefix=PLAIN

#define _GNU_SOURCE
#include <pthread.h>
#include <threads.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long total, n;

#ifdef STRUCT
struct registry { pthread_mutex_t m; long n; };
struct registry reg = { PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP, 0 };
#else
struct registry { pthread_mutex_t m; long n; };
struct registry reg = { PTHREAD_MUTEX_INITIALIZER, 0 };
#endif

// STRUCT: remark: wtc: delegated section add #1 on lock not delegated: (c) takes a pthread or C11 mutex the pass cannot prove is not recursive: the global reg holds a pthread mutex initialised to other than zeros (a recursive initialiser)
// C11: remark: wtc: delegated section add #1 on lock not delegated: (c) takes a pthread or C11 mutex the pass cannot prove is not recursive: the module makes a C11 mutex that may be recursive (mtx_init)
// NOMAIN: remark: wtc: delegated section add #1 on lock not delegated: (c) takes a pthread or C11 mutex the pass cannot prove is not recursive: files the pass does not see may make a pthread or C11 mutex recursive
// PLAIN: remark: wtc: delegated section add #1 on lock delegated
void add(long x) {
    pthread_mutex_lock(&lock);
    pthread_mutex_lock(&reg.m);
    reg.n++;
    pthread_mutex_unlock(&reg.m);
    total += x;
    pthread_mutex_unlock(&lock);
}

#ifdef C11
mtx_t inner;
void setup(void) { mtx_init(&inner, mtx_plain | mtx_recursive); }
#endif

#ifndef NOMAIN
int main(void) { return 0; }
#endif
