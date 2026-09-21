// RUN: wtc-opt %s --wtc-specialize 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// The tag arrives as a run-time value rather than a constant, so there is no
// constant to rewrite. Anything else here would mean writing into a value
// the program computes.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.global external @q = #cir.zero : !queue_i32 {wtc.topology = #wtc.topology<spsc>}
  cir.func @init(%tag: !s32i) {
    %0 = cir.get_global @q : !cir.ptr<!queue_i32>
    cir.call @ctor(%0, %tag) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.return
  }
}

// CHECK: not specialised: the topology tag is not a constant
// IR-NOT: wtc.specialized
