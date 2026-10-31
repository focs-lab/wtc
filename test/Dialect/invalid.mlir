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

// -----

// So is a lock batch operation's: it lowers to a call of the mutex and the
// batch.

!s32i = !cir.int<s, 32>
!m = !cir.struct<"m" {data !s32i}>
!b = !cir.struct<"b" {data !s32i}>
module {
  cir.func private @step(!cir.ptr<!m>) [#cir.annotation<"wtc_mutex_batch_step">]
  cir.func @step_arity(%mp: !cir.ptr<!m>, %bp: !cir.ptr<!b>) {
    %h = builtin.unrealized_conversion_cast %mp : !cir.ptr<!m> to !wtc.mutex
    // expected-error @+1 {{fallback_impl @step takes 1 arguments, but lowering passes 2}}
    "wtc.lock_batch_step"(%h, %bp) {fallback_impl = @step} : (!wtc.mutex, !cir.ptr<!b>) -> ()
    cir.return
  }
}

// -----

// A waiting pop is a transfer: it moves the element into the program's
// object, and only it takes a limit.

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func @wait_not_transfer(%qp: !cir.ptr<!q>, %obj: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{a waiting pop moves the element into its `out` operand: it must be a transfer}}
    "wtc.queue_pop"(%h, %obj) {wait} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!s64i = !cir.int<s, 64>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func @limit_without_wait(%qp: !cir.ptr<!q>, %obj: !cir.ptr<!s32i>, %limit: !s64i) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{only a waiting pop takes a `timeout`}}
    "wtc.queue_pop"(%h, %obj, %limit) {transfer} : (!wtc.queue<!q>, !cir.ptr<!s32i>, !s64i) -> !cir.bool
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func private @try_pop_into(!cir.ptr<!q>, !cir.ptr<!s32i>) -> !cir.bool [#cir.annotation<"wtc_queue_try_pop_into">]
  // A waiting pop lowers to pop_wait, not to the non-blocking pop.
  cir.func @wait_lowers_to_try_pop(%qp: !cir.ptr<!q>, %obj: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{fallback_impl @try_pop_into is not annotated wtc_queue_pop_wait or wtc_impl_*_pop_wait}}
    "wtc.queue_pop"(%h, %obj) {fallback_impl = @try_pop_into, transfer, wait} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> !cir.bool
    cir.return
  }
}

// -----

// A push that waits for room, overruns or may decline exists only on a
// bounded queue, and is one of the three.
!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func @wait_unbounded(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{waits, overruns or may decline only on a bounded queue}}
    "wtc.queue_push"(%h, %v) {wait} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func @wait_and_overrun(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{is at most one of a waiting push, an overrunning push and a try_push}}
    "wtc.queue_push"(%h, %v) {bounded, wait, overrun} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> ()
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func private @push(!cir.ptr<!q>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_push">]
  // A try_push lowers to the runtime's try_push, which answers.
  cir.func @try_push_lowers_to_push(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{fallback_impl @push is not annotated wtc_queue_try_push or wtc_impl_*_try_push}}
    %ok = "wtc.queue_push"(%h, %v) {fallback_impl = @push, bounded} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> !cir.bool
    cir.return
  }
}

// -----

// A batch (shim/c_queue.h) waits on a bounded queue and answers how many it
// moved; operands after the value belong to a batch alone.
!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func @batch_unbounded(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>, %n: !s32i) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{a batch waits while a bounded queue is full and answers how many it appended}}
    %k = "wtc.queue_push"(%h, %v, %n) {batch, wait} : (!wtc.queue<!q>, !cir.ptr<!s32i>, !s32i) -> !s32i
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func @more_without_batch(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>, %n: !s32i) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{takes operands after the value only as a batch}}
    "wtc.queue_push"(%h, %v, %n) {bounded, wait} : (!wtc.queue<!q>, !cir.ptr<!s32i>, !s32i) -> ()
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func @pop_batch_not_waiting(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>, %n: !s32i) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{a batch is a waiting transfer from a bounded queue}}
    %k = "wtc.queue_pop"(%h, %v, %n, %n) {batch, transfer, bounded} : (!wtc.queue<!q>, !cir.ptr<!s32i>, !s32i, !s32i) -> !s32i
    cir.return
  }
}

// -----

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
module {
  cir.func private @pop_wait(!cir.ptr<!q>, !cir.ptr<!s32i>, !s32i, !s32i) -> !s32i [#cir.annotation<"wtc_queue_pop_wait">]
  // A batched pop lowers to the runtime's batched pop.
  cir.func @pop_batch_lowers_to_pop_wait(%qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>, %n: !s32i) {
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    // expected-error @+1 {{fallback_impl @pop_wait is not annotated wtc_queue_pop_batch or wtc_impl_*_pop_batch}}
    %k = "wtc.queue_pop"(%h, %v, %n, %n) {fallback_impl = @pop_wait, batch, transfer, wait, bounded} : (!wtc.queue<!q>, !cir.ptr<!s32i>, !s32i, !s32i) -> !s32i
    cir.return
  }
}
