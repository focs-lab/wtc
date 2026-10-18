// RUN: wtc-opt %s --select-queue-impl --mlir-print-op-on-diagnostic=false 2>&1 | FileCheck %s

// Two globals of the same type, wtc::queue<int>, reduced from real output.
// @q1 is SPSC and @q2 is MPSC (the verdicts are written here directly, as
// --queue-ownership-analysis would). @q1's storage, construction, push,
// pop and destructor registration are retargeted to spsc_queue<int>; @q2
// keeps every one of them. The two share a destructor, so retargeting must
// follow @q1's registration, not every reference to that destructor: the
// version in 253a2bc retargeted both and would have destroyed @q2, still a
// wtc::queue, with spsc_queue's destructor.
//
// The anchor exists only to make the substitute's functions appear. It is
// erased, along with its @llvm.used entry and the address the shim's
// constructor discards.

!s32i = !cir.int<s, 32>
!u8i = !cir.int<u, 8>
!void = !cir.void
!queue = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
!spsc = !cir.struct<"wtc::runtime::spsc_queue<int, 1024UL>" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>
!shared_ptr = !cir.struct<"std::shared_ptr<int>" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>

module attributes {cir.record_layouts = {"wtc::runtime::spsc_queue<int, 1024UL>" = #cir.record_layout<arg_passing_kind = cannot_pass_in_regs, has_trivial_dtor = false, record_align = 64>}} {
  cir.global "private" hidden external @__dso_handle : !u8i
  cir.func private @__cxa_atexit(!cir.ptr<!cir.func<(!cir.ptr<!void>)>>, !cir.ptr<!void>, !cir.ptr<!u8i>) -> !s32i

  cir.global external @q1 = #cir.zero : !queue {alignment = 8 : i64, wtc.queue_kind = "SPSC"}
  cir.global external @q2 = #cir.zero : !queue {alignment = 8 : i64, wtc.queue_kind = "MPSC"}
  cir.global "private" external @v : !s32i

  cir.func private @queue_ctor(!cir.ptr<!queue>) [#cir.annotation<"wtc_queue_construction">]
  cir.func private @queue_dtor(!cir.ptr<!queue>)
  cir.func private @queue_push(!cir.ptr<!queue>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">]
  cir.func private @queue_try_pop(!cir.ptr<!queue>) -> !shared_ptr [#cir.annotation<"wtc_queue_try_pop">]

  cir.func private @spsc_ctor(!cir.ptr<!spsc>) [#cir.annotation<"wtc_impl_spsc_construction">]
  cir.func private @spsc_dtor(!cir.ptr<!spsc>) [#cir.annotation<"wtc_impl_spsc_dtor">]
  cir.func private @spsc_push(!cir.ptr<!spsc>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_impl_spsc_push">]
  cir.func private @spsc_try_pop(!cir.ptr<!spsc>) -> !shared_ptr [#cir.annotation<"wtc_impl_spsc_try_pop">]

  cir.func @anchor(%v: !cir.ptr<!s32i>) [#cir.annotation<"wtc_anchor">] {
    cir.return
  }
  cir.global "private" appending @llvm.used = #cir.const_array<[#cir.global_view<@anchor> : !cir.ptr<!void>]> : !cir.array<!cir.ptr<!void> x 1> {section = "llvm.metadata"}

  cir.func internal private @__cxx_global_var_init() {
    %0 = cir.get_global @q1 : !cir.ptr<!queue>
    cir.call @queue_ctor(%0) : (!cir.ptr<!queue>) -> ()
    %1 = cir.get_global @q1 : !cir.ptr<!queue>
    %2 = cir.get_global @queue_dtor : !cir.ptr<!cir.func<(!cir.ptr<!queue>)>>
    %3 = cir.cast bitcast %2 : !cir.ptr<!cir.func<(!cir.ptr<!queue>)>> -> !cir.ptr<!cir.func<(!cir.ptr<!void>)>>
    %4 = cir.cast bitcast %1 : !cir.ptr<!queue> -> !cir.ptr<!void>
    %5 = cir.get_global @__dso_handle : !cir.ptr<!u8i>
    %6 = cir.call @__cxa_atexit(%3, %4, %5) : (!cir.ptr<!cir.func<(!cir.ptr<!void>)>>, !cir.ptr<!void>, !cir.ptr<!u8i>) -> !s32i
    cir.return
  }

  cir.func internal private @__cxx_global_var_init.1() {
    %0 = cir.get_global @q2 : !cir.ptr<!queue>
    cir.call @queue_ctor(%0) : (!cir.ptr<!queue>) -> ()
    %1 = cir.get_global @q2 : !cir.ptr<!queue>
    %2 = cir.get_global @queue_dtor : !cir.ptr<!cir.func<(!cir.ptr<!queue>)>>
    %3 = cir.cast bitcast %2 : !cir.ptr<!cir.func<(!cir.ptr<!queue>)>> -> !cir.ptr<!cir.func<(!cir.ptr<!void>)>>
    %4 = cir.cast bitcast %1 : !cir.ptr<!queue> -> !cir.ptr<!void>
    %5 = cir.get_global @__dso_handle : !cir.ptr<!u8i>
    %6 = cir.call @__cxa_atexit(%3, %4, %5) : (!cir.ptr<!cir.func<(!cir.ptr<!void>)>>, !cir.ptr<!void>, !cir.ptr<!u8i>) -> !s32i
    cir.return
  }

  cir.func @use_q1() {
    %queue = cir.get_global @q1 : !cir.ptr<!queue>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!queue> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) {fallback_impl = @queue_push, source_queue_type = !cir.ptr<!queue>, wtc.queue_kind = "SPSC"} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    %popped = "wtc.queue_pop"(%qcast) {fallback_impl = @queue_try_pop, source_queue_type = !cir.ptr<!queue>, wtc.queue_kind = "SPSC"} : (!wtc.queue<!s32i>) -> !shared_ptr
    cir.return
  }

  cir.func @use_q2() {
    %queue = cir.get_global @q2 : !cir.ptr<!queue>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!queue> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) {fallback_impl = @queue_push, source_queue_type = !cir.ptr<!queue>, wtc.queue_kind = "MPSC"} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// CHECK-DAG: remark: substituted "wtc::runtime::spsc_queue<int, 1024UL>"
// CHECK-DAG: remark: not substituted: the queue is MPSC

// CHECK-NOT: @llvm.used
// CHECK-NOT: cir.func @anchor

// CHECK: [[SPSC:!rec[A-Za-z0-9_]*]] = !cir.struct<"wtc::runtime::spsc_queue<int, 1024UL>"
// CHECK: cir.global external @q1 = #cir.zero : [[SPSC]] {alignment = 64 : i64
// CHECK: cir.global external @q2 = #cir.zero : !rec_wtc3A3Aqueue3Cint3E {alignment = 8 : i64

// CHECK-LABEL: cir.func internal private @__cxx_global_var_init()
// CHECK: cir.get_global @q1 : !cir.ptr<[[SPSC]]>
// CHECK-NEXT: cir.call @spsc_ctor(
// CHECK: cir.get_global @spsc_dtor : !cir.ptr<!cir.func<(!cir.ptr<[[SPSC]]>)>>
// CHECK: cir.call @__cxa_atexit(

// CHECK-LABEL: cir.func internal private @__cxx_global_var_init.1()
// CHECK: cir.get_global @q2 : !cir.ptr<!rec_wtc3A3Aqueue3Cint3E>
// CHECK-NEXT: cir.call @queue_ctor(
// CHECK: cir.get_global @queue_dtor : !cir.ptr<!cir.func<(!cir.ptr<!rec_wtc3A3Aqueue3Cint3E>)>>

// CHECK-LABEL: cir.func @use_q1()
// CHECK: "wtc.queue_push"{{.*}}fallback_impl = @spsc_push, source_queue_type = !cir.ptr<[[SPSC]]>
// CHECK: "wtc.queue_pop"{{.*}}fallback_impl = @spsc_try_pop, source_queue_type = !cir.ptr<[[SPSC]]>

// CHECK-LABEL: cir.func @use_q2()
// CHECK: "wtc.queue_push"{{.*}}fallback_impl = @queue_push
