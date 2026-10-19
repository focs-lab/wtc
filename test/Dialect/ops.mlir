// RUN: wtc-opt %s | wtc-opt | FileCheck %s

// Every operation in a valid form, including a fallback_impl that names a
// substitute implementation rather than the shim, verifies and survives
// printing and parsing again.

!s32i = !cir.int<s, 32>
!q = !cir.struct<"q" {data !s32i}>
!m = !cir.struct<"m" {data !s32i}>
!sp = !cir.struct<"sp" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>

module {
  cir.func private @lock(!cir.ptr<!m>) [#cir.annotation<"wtc_mutex_lock">]
  cir.func private @unlock(!cir.ptr<!m>) [#cir.annotation<"wtc_mutex_unlock">]
  cir.func private @spsc_push(!cir.ptr<!q>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_impl_spsc_push">]
  cir.func private @try_pop(!cir.ptr<!q>) -> !sp [#cir.annotation<"wtc_queue_try_pop">]
  cir.func private @try_pop_sret(!cir.ptr<!sp> {llvm.sret = !sp}, !cir.ptr<!q>) [#cir.annotation<"wtc_queue_try_pop">]

  cir.func @all(%mp: !cir.ptr<!m>, %qp: !cir.ptr<!q>, %v: !cir.ptr<!s32i>, %slot: !cir.ptr<!sp>) {
    %mh = builtin.unrealized_conversion_cast %mp : !cir.ptr<!m> to !wtc.mutex
    %h = builtin.unrealized_conversion_cast %qp : !cir.ptr<!q> to !wtc.queue<!q>
    "wtc.lock"(%mh) {fallback_impl = @lock} : (!wtc.mutex) -> ()
    "wtc.unlock"(%mh) {fallback_impl = @unlock} : (!wtc.mutex) -> ()
    "wtc.queue_push"(%h, %v) {fallback_impl = @spsc_push, source_queue_type = !cir.ptr<!q>} : (!wtc.queue<!q>, !cir.ptr<!s32i>) -> ()
    %r = "wtc.queue_pop"(%h) {fallback_impl = @try_pop} : (!wtc.queue<!q>) -> !sp
    "wtc.queue_pop"(%h, %slot) {fallback_impl = @try_pop_sret} : (!wtc.queue<!q>, !cir.ptr<!sp>) -> ()
    cir.return
  }
}

// CHECK-LABEL: cir.func @all
// CHECK: "wtc.lock"
// CHECK: "wtc.unlock"
// CHECK: "wtc.queue_push"({{.*}}) <{fallback_impl = @spsc_push, source_queue_type = !cir.ptr<{{.*}}>}>
// CHECK: "wtc.queue_pop"({{.*}}) <{fallback_impl = @try_pop}>
// CHECK: "wtc.queue_pop"({{.*}}, {{.*}}) <{fallback_impl = @try_pop_sret}>
