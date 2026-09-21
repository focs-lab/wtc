// RUN: wtc-opt %s --wtc-specialize 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// A verdict with nothing to act on: the queue is constructed somewhere this
// module cannot see. Refusing is the only safe answer, and it must say so
// rather than pass silently, because a silent refusal and a successful
// rewrite look identical from outside.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.global external @q = #cir.zero : !queue_i32 {wtc.topology = #wtc.topology<spsc>}
}

// CHECK: not specialised: no constructor call found for this queue
// IR-NOT: wtc.specialized
