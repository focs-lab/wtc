// REQUIRES: clangir
// RUN: %cir_cxx %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc | FileCheck %s

// The prototype's mutex example (8597095^, examples/mutex_basic.cpp). With
// no value returned anywhere, every operand is where the passes expect it,
// and lifting turns both calls into dialect operations.

#include "mutex.hpp"

void f(wtc::mutex &m) {
    m.lock();
    m.unlock();
}

int main() {
    wtc::mutex m;
    f(m);
    return 0;
}

// CHECK-LABEL: cir.func {{.*}}@_Z1fRN3wtc5mutexE
// CHECK: wtc.lock
// CHECK: wtc.unlock
