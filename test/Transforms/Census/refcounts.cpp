// The census pass plants wtc_rc_note at the entry of each of libstdc++'s
// shared_ptr count functions the module defines, with the operation's
// number, and declares the function once; nothing else is touched.
//
// RUN: %clangxx -std=c++17 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-census -o %t.out.cir 2>&1 | FileCheck %s --check-prefix=REMARK
// RUN: FileCheck %s < %t.out.cir
//
// The base's _M_destroy and the in-place block's override are both counted
// as destruction; only one of them runs for a given block. The base's
// constructor is the block's making.
// REMARK: remark: wtc-census: reference-count functions instrumented: 8 (copy 1, release 1, lock 1, weak copy 1, weak release 1, destroy 2, make 1)
//
// CHECK-LABEL: cir.func {{.*}}@main(
// CHECK-NOT: @wtc_rc_note
// CHECK: cir.return
// CHECK-LABEL: cir.func {{.*}}@_ZNSt16_Sp_counted_baseILN9__gnu_cxx12_Lock_policyE2EE15_M_add_ref_copyEv(
// CHECK-NEXT: %[[C:.*]] = cir.cast bitcast %arg0 : !cir.ptr<{{.*}}> -> !cir.ptr<!void>
// CHECK-NEXT: %[[OP:.*]] = cir.const #cir.int<0> : !s32i
// CHECK-NEXT: cir.call @wtc_rc_note(%[[C]], %[[OP]]) : (!cir.ptr<!void>, !s32i) -> ()
// CHECK-LABEL: cir.func {{.*}}@_ZNSt16_Sp_counted_baseILN9__gnu_cxx12_Lock_policyE2EE10_M_releaseEv(
// CHECK-NEXT: cir.cast bitcast %arg0
// CHECK-NEXT: cir.const #cir.int<1> : !s32i
// CHECK-NEXT: cir.call @wtc_rc_note
// CHECK-LABEL: cir.func {{.*}}@_ZNSt16_Sp_counted_baseILN9__gnu_cxx12_Lock_policyE2EE19_M_release_last_useEv(
// CHECK-NOT: @wtc_rc_note
// CHECK: cir.return
// CHECK-LABEL: cir.func {{.*}}@_ZNSt16_Sp_counted_baseILN9__gnu_cxx12_Lock_policyE2EEC2Ev(
// CHECK-NEXT: cir.cast bitcast %arg0
// CHECK-NEXT: cir.const #cir.int<6> : !s32i
// CHECK-NEXT: cir.call @wtc_rc_note
// CHECK: cir.func extern_weak private @wtc_rc_note(!cir.ptr<!void>, !s32i)

#include <memory>

int main() {
    auto p = std::make_shared<int>(1);
    auto q = p;
    std::weak_ptr<int> w = p;
    auto l = w.lock();
    return *q + *l - 2;
}
