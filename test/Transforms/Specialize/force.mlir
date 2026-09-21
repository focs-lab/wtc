// RUN: wtc-opt %s --wtc-specialize 2>&1 >%t.out | FileCheck %s --check-prefix=NONE --allow-empty
// RUN: FileCheck %s --check-prefix=NOIR < %t.out
// RUN: wtc-opt %s --wtc-specialize=force-spsc=q 2>&1 >%t2.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t2.out

// The forcing option, which exists only to build a program that races on
// purpose, so that the run-time contract check can be shown to fire. Without
// the option the same module is left alone, which is what the first two
// lines assert: the option is the only thing that overrides a verdict.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.global external @q = #cir.zero : !queue_i32 {wtc.topology = #wtc.topology<mpmc>}
  cir.func @init() {
    %0 = cir.get_global @q : !cir.ptr<!queue_i32>
    %1 = cir.const #cir.int<0> : !s32i
    cir.call @ctor(%0, %1) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.return
  }
}

// Nothing at all is said without the option, so the check must be allowed to
// see an empty stream; and "not specialised" contains "specialised", so the
// pattern has to be anchored to the whole message.
// NONE-NOT: constructor tag rewritten
// NOIR-NOT: wtc.specialized

// CHECK: specialised: constructor tag rewritten to spsc
// IR: cir.const #cir.int<1> : !s32i
