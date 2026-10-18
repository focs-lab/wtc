// RUN: wtc-opt %s | wtc-opt | FileCheck %s

// Every op in a valid form, including a fallback_impl that names a
// substitute implementation rather than the shim, survives printing and
// parsing again.

!s32i = !cir.int<s, 32>
!mutex = !cir.struct<"wtc::mutex" {data !s32i}>
!shared_ptr_i32 = !cir.struct<"std::shared_ptr<int>" {data !cir.ptr<!s32i>, data !cir.ptr<!s32i>}>

module {
  cir.func private @lock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_lock">]
  cir.func private @unlock(!cir.ptr<!mutex>) [#cir.annotation<"wtc_mutex_unlock">]
  cir.func private @spsc_push(!cir.ptr<!s32i>, !cir.ptr<!s32i>) [#cir.annotation<"wtc_impl_spsc_push">]
  cir.func private @try_pop(!cir.ptr<!s32i>) -> !shared_ptr_i32 [#cir.annotation<"wtc_queue_try_pop">]
  cir.func private @try_pop_sret(!cir.ptr<!shared_ptr_i32> {llvm.sret = !shared_ptr_i32}, !cir.ptr<!s32i>) [#cir.annotation<"wtc_queue_try_pop">]

  cir.func @all(%mp: !cir.ptr<!mutex>, %qp: !cir.ptr<!s32i>, %v: !cir.ptr<!s32i>, %slot: !cir.ptr<!shared_ptr_i32>) {
    %m = builtin.unrealized_conversion_cast %mp : !cir.ptr<!mutex> to !wtc.mutex
    %q = builtin.unrealized_conversion_cast %qp : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.lock"(%m) {fallback_impl = @lock} : (!wtc.mutex) -> ()
    "wtc.unlock"(%m) {fallback_impl = @unlock} : (!wtc.mutex) -> ()
    "wtc.queue_push"(%q, %v) {fallback_impl = @spsc_push} : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    %r = "wtc.queue_pop"(%q) {fallback_impl = @try_pop} : (!wtc.queue<!s32i>) -> !shared_ptr_i32
    "wtc.queue_pop"(%q, %slot) {fallback_impl = @try_pop_sret} : (!wtc.queue<!s32i>, !cir.ptr<!shared_ptr_i32>) -> ()
    cir.return
  }
}

// CHECK-LABEL: cir.func @all
// CHECK: "wtc.lock"
// CHECK: "wtc.unlock"
// CHECK: "wtc.queue_push"({{.*}}) <{fallback_impl = @spsc_push}>
// CHECK: "wtc.queue_pop"({{.*}}) <{fallback_impl = @try_pop}> : (!wtc.queue<!s32i>) -> !rec
// CHECK: "wtc.queue_pop"({{.*}}, {{.*}}) <{fallback_impl = @try_pop_sret}> : (!wtc.queue<!s32i>, !cir.ptr<{{.*}}>) -> ()
