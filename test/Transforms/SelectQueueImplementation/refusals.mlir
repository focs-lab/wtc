// RUN: wtc-opt %s --select-queue-impl -split-input-file -verify-diagnostics

// Every way a queue is left on its original implementation says why. The
// verdicts are written here directly, as --queue-ownership-analysis would.

// A queue whose verdict is anything but SPSC is not touched.

!s32i = !cir.int<s, 32>
!queue = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  // expected-remark @+1 {{not substituted: the queue is UNKNOWN}}
  cir.global external @q = #cir.zero : !queue {wtc.queue_kind = "UNKNOWN"}
}

// -----

// The global's address goes to a function this pass knows nothing about.
// Changing the global's type would leave that function reading spsc_queue
// storage as a wtc::queue.

!s32i = !cir.int<s, 32>
!queue = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
!spsc = !cir.struct<"wtc::runtime::spsc_queue<int, 1024UL>" {data !cir.ptr<!s32i>}>
!shared_ptr = !cir.struct<"std::shared_ptr<int>" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>
module {
  // expected-remark @+1 {{not substituted: it has a use this pass cannot retarget}}
  cir.global external @q = #cir.zero : !queue {wtc.queue_kind = "SPSC"}

  cir.func private @inspect(!cir.ptr<!queue>)
  cir.func private @spsc_ctor(!cir.ptr<!spsc>) [#cir.annotation<"wtc_impl_spsc_construction">]
  cir.func private @spsc_dtor(!cir.ptr<!spsc>) [#cir.annotation<"wtc_impl_spsc_dtor">]
  cir.func private @spsc_push(!cir.ptr<!spsc>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_impl_spsc_push">]
  cir.func private @spsc_try_pop(!cir.ptr<!spsc>) -> !shared_ptr [#cir.annotation<"wtc_impl_spsc_try_pop">]

  cir.func @f() {
    %queue = cir.get_global @q : !cir.ptr<!queue>
    // expected-remark @+1 {{this use of @q is the one}}
    cir.call @inspect(%queue) : (!cir.ptr<!queue>) -> ()
    cir.return
  }
}

// -----

// An op on this queue can also reach another queue that is not SPSC, so
// retargeting it would be wrong for the other one.

!s32i = !cir.int<s, 32>
!queue = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  // expected-remark @+1 {{not substituted: an op on it can also reach a queue that is not SPSC}}
  cir.global external @q = #cir.zero : !queue {wtc.queue_kind = "SPSC"}
  cir.global "private" external @v : !s32i

  cir.func @f() {
    %queue = cir.get_global @q : !cir.ptr<!queue>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!queue> to !wtc.queue<!s32i>
    // expected-remark @+1 {{this use of @q is the one}}
    "wtc.queue_push"(%qcast, %value) {wtc.queue_kind = "MPSC"} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

// The substitute in the module is for another element type.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!queue = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
!spsc = !cir.struct<"wtc::runtime::spsc_queue<long, 1024UL>" {data !cir.ptr<!s64i>}>
!shared_ptr = !cir.struct<"std::shared_ptr<long>" {data !cir.ptr<!s64i>, data !cir.ptr<!s64i>}>
module {
  // expected-remark @+1 {{not substituted: no substitute implementation for element type '!cir.int<s, 32>' is instantiated in this module}}
  cir.global external @q = #cir.zero : !queue {wtc.queue_kind = "SPSC"}
  cir.global "private" external @v : !s32i

  cir.func private @spsc_ctor(!cir.ptr<!spsc>) [#cir.annotation<"wtc_impl_spsc_construction">]
  cir.func private @spsc_dtor(!cir.ptr<!spsc>) [#cir.annotation<"wtc_impl_spsc_dtor">]
  cir.func private @spsc_push(!cir.ptr<!spsc>, !cir.ptr<!s64i>) [#cir.annotation<"wtc_impl_spsc_push">]
  cir.func private @spsc_try_pop(!cir.ptr<!spsc>) -> !shared_ptr [#cir.annotation<"wtc_impl_spsc_try_pop">]

  cir.func @f() {
    %queue = cir.get_global @q : !cir.ptr<!queue>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!queue> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) {wtc.queue_kind = "SPSC"} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}
