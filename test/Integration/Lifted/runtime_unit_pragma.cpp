// A finding of the review of 23 Nov 2026: a #pragma pack(push, 1) around
// the file's system includes: the runtime's instantiations are a unit of
// their own, so the file is compiled as written, and the class lifts with
// what the file prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
//
// CHECK: pair 5
// CHECK: popped 1
// WHY: locked queue q_ in LQ lifted (

#pragma pack(push, 1)
#include <utility>
#include <memory>
#pragma pack(pop)
#include "Inputs/lq.h"

LQ g;

int main() {
    std::printf("pair %zu\n", sizeof(std::pair<char, int>));
    g.push(1);
    int v;
    g.pop(v);
    std::printf("popped %d\n", v);
}
