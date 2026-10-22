// RUN: wtc-opt %s --lift-cir-to-wtc --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// A queue inside a struct is carried by the one constructor call that
// builds it. Two constructor calls reach the same member -- storage
// reused by placement -- and the verdict, spsc, has no single call to be
// kept on: rewriting one call would give the object two implementations
// over its lifetime. The queue keeps the general-purpose implementation,
// and the remark says why.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
!chan = !cir.struct<"chan" {data !queue_i32, data !s32i}>
!shared_ptr_i32 = !cir.struct<"std::shared_ptr<int>" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>
!fnptr = !cir.ptr<!cir.func<(!cir.ptr<!queue_i32>)>>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.func private @push(!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">]
  cir.func private @try_pop(!cir.ptr<!queue_i32>) -> !shared_ptr_i32 [#cir.annotation<"wtc_queue_try_pop">]
  cir.func @spawn(%fn: !fnptr, %arg: !cir.ptr<!queue_i32>) [#cir.annotation<"wtc_thread_spawn">] { cir.return }
  cir.global external @s = #cir.zero : !chan
  cir.global external @v = #cir.int<1> : !s32i

  cir.func @producer(%q: !cir.ptr<!queue_i32>) {
    %v = cir.get_global @v : !cir.ptr<!s32i>
    cir.call @push(%q, %v) : (!cir.ptr<!queue_i32>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @main() {
    %s = cir.get_global @s : !cir.ptr<!chan>
    %q = cir.get_member %s[0] {name = "q"} : !cir.ptr<!chan> -> !cir.ptr<!queue_i32>
    %tag = cir.const #cir.int<0> : !s32i
    cir.call @ctor(%q, %tag) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.call @ctor(%q, %tag) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    %p = cir.get_global @producer : !fnptr
    cir.call @spawn(%p, %q) : (!fnptr, !cir.ptr<!queue_i32>) -> ()
    %got = cir.call @try_pop(%q) : (!cir.ptr<!queue_i32>) -> !shared_ptr_i32
    cir.return
  }
}

// CHECK: queue @s.q: spsc (producers: 1, consumers: 1)
// CHECK: queue @s.q keeps the general-purpose implementation: more than one constructor call may build it
