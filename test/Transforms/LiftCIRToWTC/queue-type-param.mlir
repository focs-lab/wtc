// RUN: wtc-opt %s --lift-cir-to-wtc | FileCheck %s

// The queue type's parameter is the type of the queue object itself, taken
// from the pointer the call was given. It used to be the element type,
// recovered by casting the result record and taking member zero, which hard
// coded the layout of std::shared_ptr and crashed on anything else. Nothing
// downstream needs the element type.

!s32i = !cir.int<s, 32>
!f64 = !cir.double
!queue_f64 = !cir.struct<"wtc::queue<double>" {data !cir.ptr<!f64>}>

module {
  cir.func private @push(!cir.ptr<!queue_f64>, !cir.ptr<!f64>) [#cir.annotation<"wtc_queue_push">]

  cir.func @caller(%q: !cir.ptr<!queue_f64>, %v: !cir.ptr<!f64>) {
    cir.call @push(%q, %v) : (!cir.ptr<!queue_f64>, !cir.ptr<!f64>) -> ()
    cir.return
  }
}

// The type prints through whatever alias the printer chose, so the alias is
// captured rather than spelled: hard-coding a mangled name would make this
// test about the printer.
// CHECK: ![[ALIAS:[a-zA-Z0-9_]+]] = !cir.struct<"wtc::queue<double>"
// CHECK: !wtc.queue<![[ALIAS]]>
