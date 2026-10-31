// A class used by two files, of which wtc-cc sees one. This file pushes
// and pops; the other, built as written, pushes and counts. Lifted here,
// the class's push in this file would be the runtime's and the other
// file's the deque's, over the same storage, and the linker keeps one copy
// of each inline member function for both. The class is named by the
// other file's symbols, which wtc-cc lists, and does not lift; built with
// -c, nothing tells what other files do with it, and it does not lift
// either. The program runs as written in both cases.
//
// RUN: %clangxx -std=c++17 -O1 -c %S/Inputs/other_unit_b.cpp -o %t.b.o
// RUN: %wtc_cc -O1 %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=LINKED
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: %wtc_cc -O1 -c %s -o %t.a.o 2>&1 | FileCheck %s --check-prefix=OPEN
// RUN: %clangxx %t.b.o %t.a.o -o %t.c.exe -pthread
// RUN: %t.c.exe | FileCheck %s --check-prefix=OUT
//
// LINKED: locked queue q_ in LQ<int> not lifted: (f) a file linked with this one names LQ<int>
// LINKED-NOT: lifted (
// OPEN: locked queue q_ in LQ<int> not lifted: (f) the unit is compiled to be linked with files the pass does not see
// OPEN-NOT: lifted (
// OUT: pending 6
// OUT: sum 306

#include "Inputs/other_unit.h"
#include <cstdio>

std::size_t report(LQ<int>& q);
void produce(LQ<int>& q, int n);

int main() {
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    produce(q, 3);
    std::printf("pending %zu\n", report(q));
    int v, s = 0;
    for (int i = 0; i < 6; ++i) {
        q.pop(v);
        s += v;
    }
    std::printf("sum %d\n", s);
}
