// What wtc-cc links decides what the analysis takes for the rest of the
// program: every file it links is listed with nm, as the linker finds
// them, a versioned shared object, a file named through -Wl, and a library
// -l finds in a -L directory (or one -Wl, or LIBRARY_PATH names)
// included; a response file, a file of a suffix the driver does not know
// (other.obj), the executable's symbols exported (-rdynamic, -Wl,-E), which
// the driver does not read, make the world open. Before, those forms linked a file
// no list named, and the class lifted. Each build is refused and prints
// what the program prints as written.
//
// RUN: rm -rf %t.d && mkdir -p %t.d
// RUN: %clangxx -std=c++17 -O2 -fPIC -shared %S/Inputs/escape_link_inputs_b.cpp -o %t.d/libother.so.1
// RUN: cp %t.d/libother.so.1 %t.d/libother.so
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.d/libother.so.1 -Wl,-rpath,%t.d -o %t.1.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.1.exe | FileCheck %s
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -L%t.d -l:libother.so.1 -Wl,-rpath,%t.d -o %t.2.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.2.exe | FileCheck %s
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -L%t.d -lother -Wl,-rpath,%t.d -o %t.3.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.3.exe | FileCheck %s
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_link_inputs_b.cpp -o %t.d/other.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -Wl,%t.d/other.o -o %t.4.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.4.exe | FileCheck %s
// RUN: cp %t.d/other.o %t.d/other.obj
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.d/other.obj -o %t.6.exe 2>&1 | FileCheck %s --check-prefix=OPEN
// RUN: %t.6.exe | FileCheck %s
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -Wl,-L%t.d,-lother -Wl,-rpath,%t.d -o %t.7.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.7.exe | FileCheck %s
// RUN: env LIBRARY_PATH=%t.d %wtc_cc --lift -O1 -DWTC_CHECK %s -lother -Wl,-rpath,%t.d -o %t.8.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.8.exe | FileCheck %s
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.d/other.o -rdynamic -o %t.9.exe 2>&1 | FileCheck %s --check-prefix=OPEN
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.d/other.o -Wl,-E -o %t.10.exe 2>&1 | FileCheck %s --check-prefix=OPEN
// RUN: echo %t.d/other.o > %t.d/rsp
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s @%t.d/rsp -o %t.5.exe 2>&1 | FileCheck %s --check-prefix=OPEN
// RUN: %t.5.exe | FileCheck %s
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f)
// OPEN: wtc-cc: {{.*}}: the driver cannot tell what this links
// OPEN: locked queue q_ in LQ<int> not lifted: (f) the unit is compiled to be linked with files the pass does not see

#include "Inputs/other_unit.h"
#include <cstdio>
extern "C" std::size_t their_pending(void *q);
int main() {
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    std::printf("pending %zu\n", their_pending(&q));
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
