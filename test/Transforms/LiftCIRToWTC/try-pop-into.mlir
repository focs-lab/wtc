// RUN: wtc-opt %s --lift-cir-to-wtc | FileCheck %s

// The third shape of a pop, try_pop(T&): the queue first, then the
// program's own object the element is moved into, and a bool result that
// says whether one was. The lifted operation keeps the object as its out
// operand and is marked a transfer, which tells the borrow analysis that
// nothing of the queue's is kept and the progress check that nothing is
// allocated for the element.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>

module {
  cir.func private @try_pop_into(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> !cir.bool [#cir.annotation<"wtc_queue_try_pop_into">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>, %obj: !cir.ptr<!s32i>) {
    %got = cir.call @try_pop_into(%q, %obj) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> !cir.bool
    cir.return
  }
}

// CHECK: "wtc.queue_pop"(%[[Q:.*]], %[[OBJ:.*]]) <{fallback_impl = @try_pop_into
// CHECK-SAME: transfer
// CHECK-SAME: : (!wtc.queue<{{.*}}>, !cir.ptr<{{.*}}>) -> !cir.bool
