// RUN: not wtc-opt %s --lift-cir-to-wtc 2>&1 | FileCheck %s

// An annotated push with the wrong number of operands. The pass must say so
// and fail, not index past the end of the operand list. Every operand it
// reads is read through the calling convention now, but the arity still has
// to be checked, because an annotation is a claim about a function and
// nothing verifies that the function has the shape the annotation implies.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>

module {
  cir.func private @push(!cir.ptr<!queue_i32>) [#cir.annotation<"wtc_queue_push">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>) {
    cir.call @push(%q) : (!cir.ptr<!queue_i32>) -> ()
    cir.return
  }
}

// CHECK: error: wtc: push takes the queue and one value; this call has 1 operand(s)
