// A finding of the review of 6 Nov 2026: the address passed as a variadic
// argument and read with va_arg in a helper the va_list is handed to.
// Lifted, the program printed otherwise; now the class is refused, and the
// program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_va_list_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: take hands a value that may point to it to their_pending, which has no body here

#include "Inputs/other_unit.h"
#include <cstdarg>
#include <cstdio>
extern "C" std::size_t their_pending(void *q);
static std::size_t take(va_list ap) { return their_pending(va_arg(ap, void *)); }
static std::size_t report(int n, ...) {
    va_list ap;
    va_start(ap, n);
    std::size_t r = take(ap);
    va_end(ap);
    return r;
}
int main() {
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    std::printf("pending %zu\n", report(1, static_cast<void *>(&q)));
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
