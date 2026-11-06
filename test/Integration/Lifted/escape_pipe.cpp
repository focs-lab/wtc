// A finding of the review of 6 Nov 2026: the address's bytes written to a
// pipe and read back: what is written to a file may come back from it.
// Lifted, the program printed otherwise; now the class is refused, and the
// program prints what it prints as written.
//
// RUN: %clangxx -std=c++17 -O2 -c %S/Inputs/escape_pipe_b.cpp -o %t.b.o
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s %t.b.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pending 3
// CHECK: sum 3
// WHY: locked queue q_ in LQ<int> not lifted: (f) an object of LQ<int> may be reached by code the pass does not see: main writes memory that may hold it with write

#include "Inputs/other_unit.h"
#include <cstdio>
#include <unistd.h>
extern "C" std::size_t their_pending(void *q);
int main() {
    LQ<int> q;
    for (int i = 0; i < 3; ++i) q.push(i);
    int fd[2];
    if (pipe(fd)) return 1;
    LQ<int> *p = &q;
    if (write(fd[1], &p, sizeof p) != sizeof p) return 1;
    void *back = nullptr;
    if (read(fd[0], &back, sizeof back) != sizeof back) return 1;
    std::printf("pending %zu\n", their_pending(back));
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { q.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
