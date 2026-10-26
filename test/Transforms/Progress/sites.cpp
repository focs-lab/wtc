// How the blocking sites of a function declared lock-free are reported:
// one chain per distinct site, a site inside one macro expansion once, a
// destructor run on several ways out of a scope once, and at most eight
// chains before the rest are counted.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s

#include "progress.hpp"

#include <cstdio>

long plain = 0;

// Two calls with one source location, the expansion of one macro: one note.
#define LOG_TWICE(s) do { std::fputs(s, stderr); std::fputs(s, stderr); } while (0)
// CHECK-LABEL: error: wtc: macro_sites() is declared lock-free but can block
// CHECK: note: which does input or output (fputs)
// CHECK-NOT: note: which does input or output (fputs)
WTC_LOCK_FREE void macro_sites() { LOG_TWICE("x\n"); }

// A destructor that blocks, in a scope left by two returns and by its end:
// one note.
struct Closer {
    ~Closer() { std::fputs("closed\n", stderr); }
};
// CHECK-LABEL: error: wtc: early_returns(int) is declared lock-free but can block
// CHECK: note: calls Closer::~Closer()
// CHECK: note: which does input or output (fputs)
// CHECK-NOT: note: calls Closer::~Closer()
WTC_LOCK_FREE int early_returns(int x) {
    Closer c;
    if (x > 2) return 1;
    if (x > 1) return 2;
    ++plain;
    return 0;
}

// Ten distinct sites: eight chains, then a count of the rest.
// CHECK-LABEL: error: wtc: ten_sites() is declared lock-free but can block
// CHECK-COUNT-8: note: which does input or output (fputs)
// CHECK-NOT: note: which does input or output (fputs)
// CHECK: note: and 2 more blocking sites, not shown
WTC_LOCK_FREE void ten_sites() {
    std::fputs("1", stderr);
    std::fputs("2", stderr);
    std::fputs("3", stderr);
    std::fputs("4", stderr);
    std::fputs("5", stderr);
    std::fputs("6", stderr);
    std::fputs("7", stderr);
    std::fputs("8", stderr);
    std::fputs("9", stderr);
    std::fputs("10", stderr);
}

int main() { macro_sites(); early_returns(0); ten_sites(); }
