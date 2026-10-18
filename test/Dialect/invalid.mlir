// RUN: wtc-opt %s -split-input-file -verify-diagnostics

// lock and unlock used to accept an operand of any type; the queue ops
// already constrained theirs.

!s32i = !cir.int<s, 32>
cir.func @lock_not_a_mutex(%p: !cir.ptr<!s32i>) {
  // expected-error @+1 {{operand #0 must be}}
  "wtc.lock"(%p) : (!cir.ptr<!s32i>) -> ()
  cir.return
}

// -----

// The value pushed must be a reference to the queue's element type.

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
cir.func @push_wrong_type(%qp: !cir.ptr<!s32i>, %v: !cir.ptr<!s64i>) {
  %q = builtin.unrealized_conversion_cast %qp : !cir.ptr<!s32i> to !wtc.queue<!s32i>
  // expected-error @+1 {{the pushed value points to '!cir.int<s, 64>', but the queue holds '!cir.int<s, 32>'}}
  "wtc.queue_push"(%q, %v) : (!wtc.queue<!s32i>, !cir.ptr<!s64i>) -> ()
  cir.return
}

// -----

// A pop delivers its value one way: as its result or through result_slot.

!s32i = !cir.int<s, 32>
cir.func @pop_both_ways(%qp: !cir.ptr<!s32i>, %slot: !cir.ptr<!s32i>) {
  %q = builtin.unrealized_conversion_cast %qp : !cir.ptr<!s32i> to !wtc.queue<!s32i>
  // expected-error @+1 {{must either return the popped value or write it through result_slot, not both}}
  %r = "wtc.queue_pop"(%q, %slot) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> !cir.ptr<!s32i>
  cir.return
}

// -----

!s32i = !cir.int<s, 32>
cir.func @pop_neither_way(%qp: !cir.ptr<!s32i>) {
  %q = builtin.unrealized_conversion_cast %qp : !cir.ptr<!s32i> to !wtc.queue<!s32i>
  // expected-error @+1 {{must either return the popped value or write it through result_slot, not neither}}
  "wtc.queue_pop"(%q) : (!wtc.queue<!s32i>) -> ()
  cir.return
}

// -----

// fallback_impl has to name something the op can be lowered to. Before, a
// wrong one surfaced only when the lowered call failed to verify or link.

!s32i = !cir.int<s, 32>
module {
  cir.func @push_to_nowhere(%qp: !cir.ptr<!s32i>, %v: !cir.ptr<!s32i>) {
    %q = builtin.unrealized_conversion_cast %qp : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    // expected-error @+1 {{fallback_impl @missing does not name a cir.func}}
    "wtc.queue_push"(%q, %v) {fallback_impl = @missing} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
module {
  cir.func private @not_a_push(!cir.ptr<!s32i>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_try_pop">]

  cir.func @push_to_a_pop(%qp: !cir.ptr<!s32i>, %v: !cir.ptr<!s32i>) {
    %q = builtin.unrealized_conversion_cast %qp : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    // expected-error @+1 {{fallback_impl @not_a_push is not annotated wtc_queue_push or wtc_impl_*_push}}
    "wtc.queue_push"(%q, %v) {fallback_impl = @not_a_push} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
module {
  cir.func private @try_pop(!cir.ptr<!s32i>) -> !cir.ptr<!s32i> [#cir.annotation<"wtc_queue_try_pop">]

  // With a result slot, lowering passes two arguments; this callee takes one.
  cir.func @pop_slot_mismatch(%qp: !cir.ptr<!s32i>, %slot: !cir.ptr<!s32i>) {
    %q = builtin.unrealized_conversion_cast %qp : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    // expected-error @+1 {{fallback_impl @try_pop takes 1 arguments, but lowering passes 2}}
    "wtc.queue_pop"(%q, %slot) {fallback_impl = @try_pop} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}
