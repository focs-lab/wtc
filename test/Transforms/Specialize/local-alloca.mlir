// RUN: wtc-opt %s --wtc-specialize 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// The same for a queue on the stack. The root is the alloca, and the tag
// still arrives at the constructor as a constant operand.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.func @main() {
    %q = cir.alloca "q" align(8) : !cir.ptr<!queue_i32> {wtc.topology = #wtc.topology<spsc>}
    %1 = cir.const #cir.int<0> : !s32i
    cir.call @ctor(%q, %1) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.return
  }
}

// CHECK: specialised: constructor tag rewritten to spsc
// IR: cir.const #cir.int<1> : !s32i
// IR-NEXT: cir.call @ctor
