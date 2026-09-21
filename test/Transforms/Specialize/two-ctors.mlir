// RUN: wtc-opt %s --wtc-specialize 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// Two constructor calls reach one object, which happens when storage is
// reused by placement. Rewriting one of them would give the object two
// different implementations over its lifetime, so the pass rewrites
// neither.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.global external @q = #cir.zero : !queue_i32 {wtc.topology = #wtc.topology<spsc>}
  cir.func @init() {
    %0 = cir.get_global @q : !cir.ptr<!queue_i32>
    %1 = cir.const #cir.int<0> : !s32i
    cir.call @ctor(%0, %1) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    %2 = cir.get_global @q : !cir.ptr<!queue_i32>
    %3 = cir.const #cir.int<0> : !s32i
    cir.call @ctor(%2, %3) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.return
  }
}

// CHECK: not specialised: 2 constructor calls reach this queue
// IR-NOT: wtc.specialized
