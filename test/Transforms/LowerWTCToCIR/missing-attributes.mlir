// RUN: wtc-opt %s --lower-wtc-to-cir -verify-diagnostics

// fallback_impl and the source type are optional on every wtc op, and the
// ownership-analysis tests build ops without them. Lowering used to
// dereference both unconditionally and crash. It now names the op and says
// which attribute is missing.

!s32i = !cir.int<s, 32>
!mutex = !cir.struct<"wtc::mutex" {data !s32i}>
module {
  cir.global "private" external @q : !s32i
  cir.global "private" external @v : !s32i

  cir.func private @lock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_lock">]
  cir.func private @push(!cir.ptr<!s32i>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">]

  cir.func @push_without_source_type() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    // expected-error @+1 {{'wtc.queue_push' op cannot be lowered: it carries no source type}}
    "wtc.queue_push"(%qcast, %value) {fallback_impl = @push} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @pop_without_anything() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    // expected-error @+1 {{'wtc.queue_pop' op cannot be lowered: it carries no fallback_impl}}
    %r = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @lock_without_fallback(%m: !cir.ptr<!mutex>) {
    %mcast = builtin.unrealized_conversion_cast %m : !cir.ptr<!mutex> to !wtc.mutex
    // expected-error @+1 {{'wtc.lock' op cannot be lowered: it carries no fallback_impl}}
    "wtc.lock"(%mcast) {source_mutex_type = !cir.ptr<!mutex>} : (!wtc.mutex) -> ()
    cir.return
  }
}
