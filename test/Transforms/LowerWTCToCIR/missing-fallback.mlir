// RUN: not wtc-opt %s --lower-wtc-to-cir 2>&1 | FileCheck %s

// An operation with no record of the call it came from cannot be lowered:
// there is no callee to call and no type to cast back to. Every operation in
// the analysis tests is built this way, and lowering used to dereference the
// attribute regardless and take the process with it. A diagnostic is the
// whole fix, and it matters because the specialisation passes to come will
// build operations the same way.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>

module {
  cir.func @caller(%q: !cir.ptr<!queue_i32>, %v: !cir.ptr<!s32i>) {
    %c = builtin.unrealized_conversion_cast %q : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    "wtc.queue_push"(%c, %v) : (!wtc.queue<!queue_i32>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK: error: wtc: cannot lower an operation that carries no fallback implementation
