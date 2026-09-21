// The mutex path from source: lock and unlock lift, the queue passes leave
// them alone, lowering restores the original calls, and the binary runs.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %examples/mutex_basic.cpp -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc | FileCheck %s --check-prefix=LIFTED
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --lower-wtc-to-cir --reconcile-unrealized-casts | FileCheck %s --check-prefix=LOWERED
// RUN: %wtc_cc %examples/mutex_basic.cpp -o %t.exe
// RUN: %t.exe
//
// LIFTED: "wtc.lock"({{.*}}) <{fallback_impl = @_ZN3wtc5mutex4lockEv
// LIFTED: "wtc.unlock"({{.*}}) <{fallback_impl = @_ZN3wtc5mutex6unlockEv
//
// LOWERED-NOT: wtc.
// LOWERED: cir.call @_ZN3wtc5mutex4lockEv(
// LOWERED: cir.call @_ZN3wtc5mutex6unlockEv(
