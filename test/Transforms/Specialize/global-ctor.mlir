// RUN: wtc-opt %s --wtc-specialize 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// A global queue the analysis proved single-producer single-consumer. The
// pass rewrites the constant its constructor receives, and marks the object
// so that a later reader can tell a rewrite happened from a verdict alone.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.global external @q = #cir.zero : !queue_i32 {wtc.topology = #wtc.topology<spsc>}
  cir.func @__cxx_global_var_init() {
    %0 = cir.get_global @q : !cir.ptr<!queue_i32>
    %1 = cir.const #cir.int<0> : !s32i
    cir.call @ctor(%0, %1) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.return
  }
}

// CHECK: specialised: constructor tag rewritten to spsc

// IR: cir.global external @q {{.*}}wtc.specialized = #wtc.topology<spsc>
// IR: cir.const #cir.int<1> : !s32i
// IR-NEXT: cir.call @ctor
