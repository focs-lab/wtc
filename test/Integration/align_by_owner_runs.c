/* The only test that runs a program this project has changed the layout of.

   Every other alignment test reads IR. That proves the attribute was set; it
   does not prove the attribute survives lowering, that the linker honours
   it, or that the program still computes what it did. This one compiles,
   transforms, lowers, links, runs, and asks the running program where its
   two counters actually landed.

   RUN: %clang -std=gnu11 -fclangir -emit-cir %s -o %t.cir
   RUN: wtc-opt %t.cir --wtc-align-by-owner -o %t.wtc.cir 2>%t.remarks
   RUN: FileCheck %s --check-prefix=REMARK < %t.remarks
   RUN: cir-translate --cir-to-llvmir %t.wtc.cir -o %t.raw.ll
   RUN: sed '/^target datalayout/d' %t.raw.ll > %t.ll
   RUN: FileCheck %s --check-prefix=IR < %t.ll
   RUN: %clang %t.ll -o %t.exe -pthread
   RUN: %t.exe | FileCheck %s --check-prefix=OUT
   RUN: %if tsan %{ %clang -O1 -fsanitize=thread -mllvm -force-attribute=sanitize_thread -c %t.ll -o %t.tsan.o %}
   RUN: %if tsan %{ %tsan_cxx -fsanitize=thread %t.tsan.o -o %t.tsan.exe -pthread %}
   RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,SUM %}

   The same program without the pass is the control for the answer. Not for
   the placement: whether the linker puts two 8-byte globals in one line
   without being told is its choice, so the control cannot be required to
   print either line.

   RUN: cir-translate --cir-to-llvmir %t.cir -o %t.orig.raw.ll
   RUN: sed '/^target datalayout/d' %t.orig.raw.ll > %t.orig.ll
   RUN: %clang %t.orig.ll -o %t.orig.exe -pthread
   RUN: %t.orig.exe | FileCheck %s --check-prefix=SUM

   REMARK: raised the alignment of @hits from 8 to 64
   REMARK: raised the alignment of @misses from 8 to 64

   IR-DAG: @hits = {{.*}}align 64
   IR-DAG: @misses = {{.*}}align 64

   OUT: sum 2000000
   OUT-NEXT: separate lines
   TSAN-NOT: ThreadSanitizer
   SUM: sum 2000000
*/

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

long hits;
long misses;

static void *count_hits(void *arg) {
    for (int i = 0; i < 1000000; ++i) ++hits;
    return 0;
}
static void *count_misses(void *arg) {
    for (int i = 0; i < 1000000; ++i) ++misses;
    return 0;
}

int main(void) {
    pthread_t a, b;
    pthread_create(&a, 0, count_hits, 0);
    pthread_create(&b, 0, count_misses, 0);
    pthread_join(a, 0);
    pthread_join(b, 0);
    printf("sum %ld\n", hits + misses);
    printf("%s lines\n",
           (uintptr_t)&hits / 64 == (uintptr_t)&misses / 64 ? "one of the" : "separate");
    return 0;
}
