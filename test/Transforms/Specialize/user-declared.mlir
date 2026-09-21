// RUN: wtc-opt %s --wtc-specialize 2>&1 >%t.out | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out

// The program states the topology itself. The pass leaves it alone, even
// though its own verdict agrees: a tag the programmer wrote is a decision,
// not a default, and overwriting it would make a declared choice
// unobservable. Comparing the two is a diagnostic worth having later.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.global external @q = #cir.zero : !queue_i32 {wtc.topology = #wtc.topology<spsc>}
  cir.func @init() {
    %0 = cir.get_global @q : !cir.ptr<!queue_i32>
    %1 = cir.const #cir.int<1> : !s32i
    cir.call @ctor(%0, %1) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.return
  }
}

// CHECK: not specialised: the program sets the tag itself (1)
// IR-NOT: wtc.specialized
