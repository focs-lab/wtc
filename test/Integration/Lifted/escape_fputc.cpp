// A finding of the review of 23 Nov 2026: the address's bytes written one
// at a time with fputc and read back with fread: fputc writes its argument
// to a file. Lifted, the program printed otherwise; now the class is
// refused, and the program prints what it prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"

LQ g;

int main() {
    g.push(1); g.push(2); g.push(3);
    FILE *f = tmpfile();
    LQ *p = &g;
    const unsigned char *b = reinterpret_cast<const unsigned char *>(&p);
    for (unsigned i = 0; i < sizeof p; ++i) std::fputc(b[i], f);
    std::rewind(f);
    std::queue<int> *back = nullptr;
    if (std::fread(&back, sizeof back, 1, f) != 1) return 1;
    std::printf("size %zu\n", back->size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
