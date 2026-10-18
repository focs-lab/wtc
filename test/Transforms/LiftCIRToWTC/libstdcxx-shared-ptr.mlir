// RUN: wtc-opt %s --lift-cir-to-wtc | FileCheck %s

// The shape of try_pop's result under libstdc++, reduced from x86-64 Linux
// output: std::shared_ptr<T>'s first member is its std::__shared_ptr base,
// and T* is that base's first member. Lifting looked only at shared_ptr's
// own first member, which is T* under libc++, so on Linux it could not
// recover the element type and refused every pop.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
!count = !cir.struct<"std::__shared_count<__gnu_cxx::_S_atomic>" {data !cir.ptr<!s32i>}>
!base = !cir.struct<"std::__shared_ptr<int, __gnu_cxx::_S_atomic>" {data !cir.ptr<!s32i>, data !count}>
!shared_ptr_i32 = !cir.struct<"std::shared_ptr<int>" {data !base}>

module {
  cir.func private @try_pop(!cir.ptr<!shared_ptr_i32> {llvm.sret = !shared_ptr_i32}, !cir.ptr<!queue_i32>) [#cir.annotation<"wtc_queue_try_pop">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>) {
    %slot = cir.alloca "tmp" align(8) : !cir.ptr<!shared_ptr_i32>
    cir.call @try_pop(%slot, %q) : (!cir.ptr<!shared_ptr_i32> {llvm.sret = !shared_ptr_i32}, !cir.ptr<!queue_i32>) -> ()
    cir.return
  }
}

// CHECK-LABEL: cir.func @caller
// CHECK: "wtc.queue_pop"({{.*}}) {{.*}} : (!wtc.queue<!s32i>, !cir.ptr<{{.*}}>) -> ()
