// Sections refused for what would differ on the server's thread, from the
// review of 23 Nov 2026: stack memory of run-time size (a variable-length
// array, alloca), the function's variadic arguments, the CPU read through
// an intrinsic, a stack frame's address, errno set by the math library, a
// thread's CPU clock; and the system's clocks, which are no obstacle.
//
// RUN: %clang -std=gnu11 -O1 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=lock -o /dev/null 2>&1 | FileCheck %s

#include <alloca.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char shared_buf[64];
static int shared_n = 16;
static long total;
static double balance = -4.0;
static void *seen;

// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: delegated section vla #1 on lock not delegated: (a) allocates stack memory of run-time size (a variable-length array or alloca), which would be the server's stack
long vla(void) {
    long sum = 0;
    pthread_mutex_lock(&lock);
    int n = shared_n;
    char copy[n];
    memcpy(copy, shared_buf, n);
    pthread_mutex_unlock(&lock);
    for (int i = 0; i < n; ++i) sum += copy[i];
    return sum;
}

// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section on_stack #1 on lock not delegated: (a) allocates stack memory of run-time size
char *on_stack(void) {
    pthread_mutex_lock(&lock);
    char *copy = alloca(32);
    memcpy(copy, shared_buf, 32);
    pthread_mutex_unlock(&lock);
    return copy[0] ? "set" : "empty";
}

// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section add_all #1 on lock not delegated: (b) reads the variadic arguments of add_all
void add_all(int n, ...) {
    pthread_mutex_lock(&lock);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; ++i) total += va_arg(ap, long);
    va_end(ap);
    pthread_mutex_unlock(&lock);
}

// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: delegated section cpu #1 on lock not delegated: (e) calls the intrinsic x86.rdtscp, which is not on the audited list
unsigned cpu(void) {
    unsigned aux;
    pthread_mutex_lock(&lock);
    (void)__rdtscp(&aux);
    total += aux;
    pthread_mutex_unlock(&lock);
    return aux;
}

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section frame #1 on lock not delegated: (a) depends on the thread that runs it: it reads the address of a stack frame
void frame(void) { pthread_mutex_lock(&lock); seen = __builtin_frame_address(0); pthread_mutex_unlock(&lock); }

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section root #1 on lock not delegated: (e) calls sqrt, which has no body here and is not on the audited list
double root(void) { pthread_mutex_lock(&lock); double r = sqrt(balance); pthread_mutex_unlock(&lock); return r; }

// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: delegated section charge #1 on lock not delegated: (a) depends on the thread that runs it: it reads a clock that may be the thread's own (clock_gettime with clock 3)
double charge(void) {
    struct timespec ts;
    pthread_mutex_lock(&lock);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    total += ts.tv_nsec;
    pthread_mutex_unlock(&lock);
    return ts.tv_sec;
}

// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: delegated section stamp #1 on lock delegated
long stamp(void) {
    struct timespec ts;
    pthread_mutex_lock(&lock);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    total += ts.tv_nsec;
    pthread_mutex_unlock(&lock);
    return total;
}

int main(void) { return 0; }
