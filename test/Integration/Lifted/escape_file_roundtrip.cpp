// A finding of the review of 23 Nov 2026: the address written to a file and
// read back with pread, off the audited list: what is written to files is
// the outside's, a file linked or not. Lifted, the program printed
// otherwise; now the class is refused, and the program prints what it
// prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"
#include <unistd.h>

LQ g;

int main() {
    g.push(1); g.push(2); g.push(3);
    FILE *f = tmpfile();
    int fd = fileno(f);
    LQ *p = &g;
    if (write(fd, &p, sizeof p) != sizeof p) return 1;
    std::queue<int> *back = nullptr;
    if (pread(fd, &back, sizeof back, 0) != sizeof back) return 1;
    // Not printf of the size itself: that would join the outside (pread) to
    // the files the object was written to, and refuse by accident.
    std::puts(back->size() == 3 ? "size 3" : "size not 3");
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
