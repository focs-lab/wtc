// A finding of the review of 23 Nov 2026: the address's bytes written with
// ostream::write and read back with istream::read: a stream insertion
// writes its first argument and its memory. Lifted, the program printed
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
#include <sstream>

LQ g;

int main() {
    g.push(1); g.push(2); g.push(3);
    std::stringstream ss;
    LQ *p = &g;
    ss.write(reinterpret_cast<const char *>(&p), sizeof p);
    std::queue<int> *back = nullptr;
    ss.read(reinterpret_cast<char *>(&back), sizeof back);
    std::printf("size %zu\n", back->size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
