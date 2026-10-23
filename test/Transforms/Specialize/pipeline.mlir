// RUN: wtc-opt %s --queue-ownership-analysis --wtc-specialize 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// The two passes in one run, which is how the driver uses them: the analysis
// decides, the specialisation acts, and nothing in between carries the
// verdict but the attribute the first leaves on the object. q1 has one
// thread on each side and q2 has two producers, so each constructor
// changes to its own implementation: the ring and the list for many
// producers.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.global "private" external @v : !s32i
  cir.global external @q1 = #cir.zero : !queue_i32
  cir.global external @q2 = #cir.zero : !queue_i32

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @p1() {
    %q = cir.get_global @q1 : !cir.ptr<!queue_i32>
    %v = cir.get_global @v : !cir.ptr<!s32i>
    %c = builtin.unrealized_conversion_cast %q : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    "wtc.queue_push"(%c, %v) : (!wtc.queue<!queue_i32>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
  cir.func @c1() {
    %q = cir.get_global @q1 : !cir.ptr<!queue_i32>
    %c = builtin.unrealized_conversion_cast %q : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    %r = "wtc.queue_pop"(%c) : (!wtc.queue<!queue_i32>) -> !cir.ptr<!s32i>
    cir.return
  }
  cir.func @p2a() {
    %q = cir.get_global @q2 : !cir.ptr<!queue_i32>
    %v = cir.get_global @v : !cir.ptr<!s32i>
    %c = builtin.unrealized_conversion_cast %q : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    "wtc.queue_push"(%c, %v) : (!wtc.queue<!queue_i32>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
  cir.func @p2b() {
    %q = cir.get_global @q2 : !cir.ptr<!queue_i32>
    %v = cir.get_global @v : !cir.ptr<!s32i>
    %c = builtin.unrealized_conversion_cast %q : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    "wtc.queue_push"(%c, %v) : (!wtc.queue<!queue_i32>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
  cir.func @c2() {
    %q = cir.get_global @q2 : !cir.ptr<!queue_i32>
    %c = builtin.unrealized_conversion_cast %q : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    %r = "wtc.queue_pop"(%c) : (!wtc.queue<!queue_i32>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @__cxx_global_var_init() {
    %a = cir.get_global @q1 : !cir.ptr<!queue_i32>
    %z = cir.const #cir.int<0> : !s32i
    cir.call @ctor(%a, %z) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    %b = cir.get_global @q2 : !cir.ptr<!queue_i32>
    %z2 = cir.const #cir.int<0> : !s32i
    cir.call @ctor(%b, %z2) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.return
  }

  cir.func @main() {
    %f1 = cir.get_global @p1 : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%f1) : (!cir.ptr<!cir.func<()>>) -> ()
    %f2 = cir.get_global @c1 : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%f2) : (!cir.ptr<!cir.func<()>>) -> ()
    %f3 = cir.get_global @p2a : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%f3) : (!cir.ptr<!cir.func<()>>) -> ()
    %f4 = cir.get_global @p2b : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%f4) : (!cir.ptr<!cir.func<()>>) -> ()
    %f5 = cir.get_global @c2 : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%f5) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// CHECK: queue @q1: spsc (producers: 1, consumers: 1)
// CHECK: queue @q2: mpsc (producers: many, consumers: 1)
// CHECK-DAG: specialised: constructor tag rewritten to spsc
// CHECK-DAG: specialised: constructor tag rewritten to mpsc

// IR: cir.global external @q1 {{.*}}wtc.specialized = #wtc.topology<spsc>
// IR: cir.global external @q2 {{.*}}wtc.specialized = #wtc.topology<mpsc>
