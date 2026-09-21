// RUN: wtc-opt %s --wtc-specialize 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// A constructor call nested inside a region, which is the shape a static
// local takes: the initialisation sits under a guard. The pass walks
// regions, so nesting must not hide the call from it.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.global external @q = #cir.zero : !queue_i32 {wtc.topology = #wtc.topology<spsc>}
  cir.func @init() {
    cir.scope {
      %0 = cir.get_global @q : !cir.ptr<!queue_i32>
      %1 = cir.const #cir.int<0> : !s32i
      cir.call @ctor(%0, %1) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    }
    cir.return
  }
}

// CHECK: specialised: constructor tag rewritten to spsc
// IR: cir.const #cir.int<1> : !s32i
