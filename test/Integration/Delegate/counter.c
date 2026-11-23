// A C program's global pthread mutex, delegated through the whole pipeline:
// four threads add through a section the pass delegates, one through a
// section it refuses (the section reads errno) and which runs under the
// mutex as written, and the sums are exact. Run as written, delegated, and
// delegated under ThreadSanitizer.
//
// RUN: %wtc_cc -O1 %s -o %t.plain.exe
// RUN: %t.plain.exe | FileCheck %s
// RUN: %wtc_cc -O1 --delegate lock %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=BUILD
// RUN: env WTC_DELEGATE_REPORT=1 %t.exe 2>&1 | FileCheck %s --check-prefixes=CHECK,SERVED
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g --delegate lock %s -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefix=BUILD %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,CHECK %}
//
// BUILD-DAG: remark: wtc: delegated section add #1 on lock delegated
// BUILD-DAG: remark: wtc: delegated section add_noting_errno #1 on lock not delegated: (a) depends on the thread that runs it: it calls __errno_location
// TSAN-NOT: ThreadSanitizer
// CHECK: total 25600250000 count 500000 ok 1
// SERVED: wtc-delegate: server 0 ran 400000 sections

#include <errno.h>
#include <pthread.h>
#include <stdio.h>

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long total, count, last_errno;

long add(long x) {
    pthread_mutex_lock(&lock);
    total += x;
    long c = ++count;
    pthread_mutex_unlock(&lock);
    return c;
}

void add_noting_errno(long x) {
    pthread_mutex_lock(&lock);
    total += x;
    ++count;
    last_errno = errno;
    pthread_mutex_unlock(&lock);
}

static void *adder(void *arg) {
    long base = (long)arg;
    long seen = 0;
    for (long i = 1; i <= 100000; ++i) {
        long c = add(base + i);
        if (c <= seen) printf("count went back\n");
        seen = c;
    }
    return 0;
}

static void *other(void *arg) {
    (void)arg;
    for (long i = 1; i <= 100000; ++i) add_noting_errno(i);
    return 0;
}

int main(void) {
    pthread_t t[5];
    for (long i = 0; i < 4; ++i) pthread_create(&t[i], 0, adder, (void *)(i * 1000));
    pthread_create(&t[4], 0, other, 0);
    for (int i = 0; i < 5; ++i) pthread_join(t[i], 0);
    // 5 * sum(1..100000) + 100000 * (0 + 1000 + 2000 + 3000)
    printf("total %ld count %ld ok %d\n", total, count, total == 25600250000L && count == 500000);
    fflush(stdout);
    return 0;
}
