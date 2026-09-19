// RUN: wtc-opt %s --queue-ownership-analysis --select-queue-impl --lower-wtc-to-cir | FileCheck %s

// A global queue proven SPSC should have its declaration, initializer,
// every get_global referencing it, and its construction/push/pop calls
// all retargeted to the anchor-instantiated wtc::spsc_queue<int,1024>
// -- not just the queue_push/queue_pop ops, but the storage itself.

!s32i = !cir.int<s, 32>
!rec_queue = !cir.record<class "wtc::queue<int>" {!s32i}>
!rec_spsc = !cir.record<class "wtc::spsc_queue<int, 1024>" {!s32i}>

module {
  cir.global external @q = #cir.zero : !rec_queue {alignment = 8 : i64}
  cir.global "private" external @v : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  // The queue<int> implementation actually in use.
  cir.func @_ZN3wtc5queueIiEC1Ev(%this: !cir.ptr<!rec_queue>) [#cir.annotation<"wtc_queue_construction">] {
    cir.return
  }
  cir.func @_ZN3wtc5queueIiE4pushERKi(%this: !cir.ptr<!rec_queue>, %value: !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">] {
    cir.return
  }
  cir.func @_ZN3wtc5queueIiE7try_popEv(%this: !cir.ptr<!rec_queue>) -> !s32i [#cir.annotation<"wtc_queue_try_pop">] {
    %r = cir.const #cir.int<0> : !s32i
    cir.return %r : !s32i
  }
  cir.func @_ZN3wtc5queueIiED1Ev(%this: !cir.ptr<!rec_queue>) special_member<#cir.cxx_dtor<!rec_queue>> {
    cir.return
  }

  // Mimics the compiler-generated __cxa_atexit registration: it obtains
  // the destructor as a bare function-pointer value via its own
  // cir.get_global, entirely independent of any use of @q. This is what
  // select-queue-impl must find and retarget on its own -- it isn't
  // reachable via @q's symbol uses at all.
  cir.func @__cxx_global_var_init() {
    %dtor = cir.get_global @_ZN3wtc5queueIiED1Ev : !cir.ptr<!cir.func<(!cir.ptr<!rec_queue>)>>
    cir.return
  }

  // The anchor: an spsc_queue<int,1024> instantiation, unused by any real
  // code path, present only so its real construction/push/pop symbols and
  // record type exist for this pass to find and reuse.
  cir.func @_ZN3wtc10spsc_queueIiLm1024EEC1Ev(%this: !cir.ptr<!rec_spsc>) [#cir.annotation<"wtc_queue_construction">] {
    cir.return
  }
  cir.func @_ZN3wtc10spsc_queueIiLm1024EE4pushERKi(%this: !cir.ptr<!rec_spsc>, %value: !cir.ptr<!s32i>) -> !cir.bool [#cir.annotation<"wtc_queue_push">] {
    %r = cir.const #cir.bool<true> : !cir.bool
    cir.return %r : !cir.bool
  }
  cir.func @_ZN3wtc10spsc_queueIiLm1024EE7try_popEv(%this: !cir.ptr<!rec_spsc>) -> !s32i [#cir.annotation<"wtc_queue_try_pop">] {
    %r = cir.const #cir.int<0> : !s32i
    cir.return %r : !s32i
  }
  cir.func @_ZN3wtc10spsc_queueIiLm1024EED1Ev(%this: !cir.ptr<!rec_spsc>) special_member<#cir.cxx_dtor<!rec_spsc>> {
    cir.return
  }

  cir.func @producer() {
    %queue = cir.get_global @q : !cir.ptr<!rec_queue>
    cir.call @_ZN3wtc5queueIiEC1Ev(%queue) : (!cir.ptr<!rec_queue>) -> ()
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!rec_queue> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) <{fallback_impl = @_ZN3wtc5queueIiE4pushERKi, source_queue_type = !cir.ptr<!rec_queue>}> : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @consumer() {
    %queue = cir.get_global @q : !cir.ptr<!rec_queue>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!rec_queue> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) <{fallback_impl = @_ZN3wtc5queueIiE7try_popEv, source_queue_type = !cir.ptr<!rec_queue>}> : (!wtc.queue<!s32i>) -> !s32i
    cir.return
  }

  cir.func @main() {
    %p = cir.get_global @producer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%p) : (!cir.ptr<!cir.func<()>>) -> ()
    %c = cir.get_global @consumer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%c) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.return
  }
}

// The printer mints its own alias for the record (ignoring the short name
// this file used on input), so capture whatever it actually picked rather
// than hardcode it.
// CHECK: [[SPSC:!rec[A-Za-z0-9_]*]] = !cir.record<class "wtc::spsc_queue<int, 1024>"

// CHECK: cir.global external @q = #cir.zero : [[SPSC]]

// The __cxa_atexit-style destructor registration, unreachable through @q's
// own symbol uses, must also be retargeted -- otherwise the original
// implementation's destructor would run against the new type's memory.
// It's declared (and so printed) before producer/consumer in this file.
// CHECK: cir.func @__cxx_global_var_init
// CHECK: cir.get_global @_ZN3wtc10spsc_queueIiLm1024EED1Ev : !cir.ptr<!cir.func<(!cir.ptr<[[SPSC]]>)>>

// CHECK: cir.func @producer
// CHECK: %[[Q:.*]] = cir.get_global @q : !cir.ptr<[[SPSC]]>
// CHECK: cir.call @_ZN3wtc10spsc_queueIiLm1024EEC1Ev(%[[Q]])
// CHECK: cir.call @_ZN3wtc10spsc_queueIiLm1024EE4pushERKi(%[[Q]]

// CHECK: cir.func @consumer
// CHECK: %[[Q2:.*]] = cir.get_global @q : !cir.ptr<[[SPSC]]>
// CHECK: cir.call @_ZN3wtc10spsc_queueIiLm1024EE7try_popEv(%[[Q2]])
