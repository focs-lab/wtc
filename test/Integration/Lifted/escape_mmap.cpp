// A finding of the review of 6 Nov 2026: the address stored through one
// shared mapping of a memory file and read through another: what mmap
// returns is the outside's. Lifted, the program printed otherwise; now the
// class is refused, and the program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_mmap_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: main takes a value that may point to it from mmap, which has no body here

#include "Inputs/other_unit.h"
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
extern "C" std::size_t their_pending(void *q);
int main() {
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    int fd = memfd_create("wtc", 0);
    if (fd < 0 || ftruncate(fd, 4096)) return 1;
    void **w = static_cast<void **>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    void **r = static_cast<void **>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    *w = &q;
    std::printf("pending %zu\n", their_pending(*r));
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
