// RUN: wtc-opt %s -split-input-file -verify-diagnostics

// fallback_impl has to name something the operation can be lowered to.
// Before these checks a wrong one surfaced only when the lowered call
// failed to verify or to link.

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func @push_to_nowhere(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{fallback_impl @missing does not name a cir.func}}
    "wtc.queue_push"(%h, %v) {fallback_impl = @missing} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func private @not_a_push(!cir.ptr<!q>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_try_pop">]
  cir.func @push_to_a_pop(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{fallback_impl @not_a_push is not annotated wtc_queue_push or wtc_impl_*_push}}
    "wtc.queue_push"(%h, %v) {fallback_impl = @not_a_push} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func private @try_pop(!cir.ptr<!q>) -> !cir.ptr<!s32i> [#cir.annotation<"wtc_queue_try_pop">]
  // With an out operand lowering passes two arguments; this callee takes one.
  cir.func @pop_out_mismatch(%qp: !cir.ptr<!q>, %slot: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{fallback_impl @try_pop takes 1 arguments, but lowering passes 2}}
    "wtc.queue_pop"(%h, %slot) {fallback_impl = @try_pop} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

!q = !cir.struct<"q" {data !cir.int<s, 32>}>
module {
  cir.func private @lock(!cir.ptr<!q>, !cir.ptr<!q>) [#cir.annotation<"wtc_mutex_lock">]
  cir.func @lock_arity(%mp: !cir.ptr<!q>) {
    %m = builtin.unrealized_conversion_cast %mp : !cir.ptr<!q> to !wtc.mutex
    // expected-error @+1 {{fallback_impl @lock takes 2 arguments, but lowering passes 1}}
    "wtc.lock"(%m) {fallback_impl = @lock} : (!wtc.mutex) -> ()
    cir.return
  }
}

// -----

// Lowering casts the handle back to source_queue_type, so it has to point
// to the object the queue type names.

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
cir.func @push_source_type(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>) {
  %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
  // expected-error @+1 {{source_queue_type '!cir.ptr<!cir.int<s, 32>>' is not a pointer to}}
  "wtc.queue_push"(%h, %v) {source_queue_type = !cir.ptr<!s32i>} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> ()
  cir.return
}

// -----

// A batch operation's fallback_impl is checked like any other's.

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
!b = !cir.struct<"b" {data !s32i}>
module {
  cir.func private @push_deferred(!cir.ptr<!q>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push_deferred">]
  cir.func @deferred_arity(%qp: !cir.ptr<!q>, %bp: !cir.ptr<!b>, %v: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{fallback_impl @push_deferred takes 2 arguments, but lowering passes 3}}
    "wtc.queue_push_deferred"(%h, %bp, %v) {fallback_impl = @push_deferred} : (!wtc.queue<!q>, !cir.ptr<!b>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}
