// A finding of the review of 23 Nov 2026: the address printed with %lu and
// parsed back with strtoul: the number strto* returns is what the string's
// memory holds. Lifted, the program printed otherwise; now the class is
// refused, and the program prints what it prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: size 3
// CHECK: sum 6
// WHY: locked queue q_ in LQ not lifted:

#include "Inputs/lq.h"
#include <cstdint>
#include <cstdlib>

LQ g;

int main() {
    g.push(1); g.push(2); g.push(3);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lu", static_cast<unsigned long>(reinterpret_cast<std::uintptr_t>(&g)));
    auto *q = reinterpret_cast<std::queue<int> *>(std::strtoul(buf, nullptr, 10));
    std::printf("size %zu\n", q->size());
    int v, s = 0;
    for (int i = 0; i < 3; ++i) { g.pop(v); s += v; }
    std::printf("sum %d\n", s);
}
