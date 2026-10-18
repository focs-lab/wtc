// RUN: wtc-opt %s --lift-cir-to-wtc -verify-diagnostics

// Operands used to be taken by index with no check, so a call to an
// annotated function with an unexpected shape read past its operands or
// lifted the wrong one. It is now named and left alone.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>

module {
  cir.func private @push(!cir.ptr<!queue_i32>) [#cir.annotation<"wtc_queue_push">]

  cir.func @caller(%q: !cir.ptr<!queue_i32>) {
    // expected-error @+2 {{expected a queue push call to take 2 arguments, got 1}}
    // expected-error @+1 {{failed to legalize operation 'cir.call'}}
    cir.call @push(%q) : (!cir.ptr<!queue_i32>) -> ()
    cir.return
  }
}
