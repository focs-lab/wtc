// A class used by two files, of which wtc-cc sees one, where the other
// file's use leaves no symbol that names the class: built at -O2, its copy
// of the member function is inlined, and it reaches the object through a
// global (GLOBAL) or a void * it is handed (VOIDPTR). The symbols nm
// lists cannot tell; this file's own code can. With a file linked, the
// class does not lift when a global another file may name holds it, or
// when its address leaves the types the pass follows. Lifted, the other
// file's inlined count read the substitute's pointer as a std::deque.
// The program runs as written in both cases.
//
// RUN: %clangxx -std=c++17 -O2 -DGLOBAL -c %S/Inputs/other_unit_inlined_b.cpp -o %t.global.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DGLOBAL %s %t.global.o -o %t.global.exe 2>&1 | FileCheck %s --check-prefix=GLOBAL
// RUN: %t.global.exe | FileCheck %s --check-prefix=OUT
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/other_unit_inlined_b.cpp -o %t.void.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.void.o -o %t.void.exe 2>&1 | FileCheck %s --check-prefix=VOIDPTR
// RUN: %t.void.exe | FileCheck %s --check-prefix=OUT
//
// GLOBAL: locked queue q_ in LQ<int> not lifted: (f) the global gq, which a file linked with this one may name, holds or points to LQ<int>
// GLOBAL-NOT: lifted (
// VOIDPTR: locked queue q_ in LQ<int> not lifted: (f) main converts a pointer to LQ<int>, or to what holds or points to it, to void *, which the pass does not follow
// VOIDPTR-NOT: lifted (
// OUT: pending 3
// OUT: sum 3

#include "Inputs/other_unit.h"
#include <cstdio>

#if defined(GLOBAL)
LQ<int> gq;
std::size_t report();
#else
extern "C" std::size_t report(void* q);
#endif

int main() {
#if defined(GLOBAL)
    LQ<int>& q = gq;
    for (int i = 0; i < 3; ++i) q.push(i);
    std::printf("pending %zu\n", report());
#else
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    std::printf("pending %zu\n", report(&q));
#endif
    int v, s = 0;
    for (int i = 0; i < 3; ++i) {
        q.pop(v);
        s += v;
    }
    std::printf("sum %d\n", s);
}
