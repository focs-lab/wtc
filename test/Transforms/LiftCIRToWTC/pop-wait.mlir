// RUN: wtc-opt %s --lift-cir-to-wtc | FileCheck %s
// RUN: wtc-opt %s --lift-cir-to-wtc --lower-wtc-to-cir --reconcile-unrealized-casts | FileCheck %s --check-prefix=ROUND

// The waiting pops, pop_wait(T&) and pop_wait_for(T&, nanoseconds): the
// queue first, then the program's own object the element is moved into,
// then, for the second, the limit. Each lifts to a queue_pop marked a
// transfer, since the element is moved out as try_pop(T&) moves it, and
// marked `wait`, which tells the progress check that it waits for another
// thread; the limit is the pop's second operand. Lowering puts the call
// back as it was.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>

module {
  cir.func private @pop_wait(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> !cir.bool [#cir.annotation<"wtc_queue_pop_wait">]
  cir.func private @pop_wait_for(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>, !s64i) -> !cir.bool [#cir.annotation<"wtc_queue_pop_wait_for">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>, %obj: !cir.ptr<!s32i>, %limit: !s64i) {
    %a = cir.call @pop_wait(%q, %obj) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> !cir.bool
    %b = cir.call @pop_wait_for(%q, %obj, %limit) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>, !s64i) -> !cir.bool
    cir.return
  }
}

// CHECK: cir.func @caller(%{{.*}}: !cir.ptr{{.*}}, %[[OBJ:.*]]: !cir.ptr<!s32i>, %[[LIMIT:.*]]: !s64i)
// CHECK: "wtc.queue_pop"(%{{.*}}, %[[OBJ]]) <{fallback_impl = @pop_wait,
// CHECK-SAME: transfer, wait}>
// CHECK-SAME: : (!wtc.queue<{{.*}}>, !cir.ptr<!s32i>) -> !cir.bool
// CHECK: "wtc.queue_pop"(%{{.*}}, %[[OBJ]], %[[LIMIT]]) <{fallback_impl = @pop_wait_for,
// CHECK-SAME: transfer, wait}>
// CHECK-SAME: : (!wtc.queue<{{.*}}>, !cir.ptr<!s32i>, !s64i) -> !cir.bool

// ROUND-NOT: wtc.queue_pop
// ROUND: cir.func @caller(%[[Q:.*]]: !cir.ptr{{.*}}, %[[OBJ:.*]]: !cir.ptr<!s32i>, %[[LIMIT:.*]]: !s64i)
// ROUND: cir.call @pop_wait(%[[Q]], %[[OBJ]])
// ROUND: cir.call @pop_wait_for(%[[Q]], %[[OBJ]], %[[LIMIT]])
