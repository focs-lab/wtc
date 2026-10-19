// RUN: wtc-opt %s --wtc-specialize 2>%t.err | FileCheck %s
// RUN: FileCheck %s --check-prefix=REMARK < %t.err

// A queue proved spsc whose every pop only borrows its element is tagged
// spsc_inline (2), provided its constructor marks the element type as one
// that may be held inline. Without the borrow proof, or without the
// marker, it stays plain spsc (1).

!s32i = !cir.int<s, 32>
!queue = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @inline_ok() [#cir.annotation<"wtc_queue_inline_ok">]
  cir.func @ctor_marked(%q: !cir.ptr<!queue>, %t: !s32i) [#cir.annotation<"wtc_queue_construction">] {
    cir.call @inline_ok() : () -> ()
    cir.return
  }
  cir.func private @ctor_unmarked(!cir.ptr<!queue>, !s32i) [#cir.annotation<"wtc_queue_construction">]

  cir.global external @borrowed = #cir.zero : !queue {wtc.borrowed, wtc.queue_id = 0 : i64, wtc.topology = #wtc.topology<spsc>}
  cir.global external @kept = #cir.zero : !queue {wtc.queue_id = 1 : i64, wtc.topology = #wtc.topology<spsc>}
  cir.global external @unmarked = #cir.zero : !queue {wtc.borrowed, wtc.queue_id = 2 : i64, wtc.topology = #wtc.topology<spsc>}

  cir.func @init() {
    %zero = cir.const #cir.int<0> : !s32i
    %a = cir.get_global @borrowed : !cir.ptr<!queue>
    cir.call @ctor_marked(%a, %zero) : (!cir.ptr<!queue>, !s32i) -> ()
    %b = cir.get_global @kept : !cir.ptr<!queue>
    cir.call @ctor_marked(%b, %zero) : (!cir.ptr<!queue>, !s32i) -> ()
    %c = cir.get_global @unmarked : !cir.ptr<!queue>
    cir.call @ctor_unmarked(%c, %zero) : (!cir.ptr<!queue>, !s32i) -> ()
    cir.return
  }
}

// CHECK-LABEL: cir.func @init
// CHECK: %[[A:.*]] = cir.const #cir.int<2> : !s32i
// CHECK-NEXT: cir.call @ctor_marked(%{{.*}}, %[[A]])
// CHECK: %[[B:.*]] = cir.const #cir.int<1> : !s32i
// CHECK-NEXT: cir.call @ctor_marked(%{{.*}}, %[[B]])
// CHECK: %[[C:.*]] = cir.const #cir.int<1> : !s32i
// CHECK-NEXT: cir.call @ctor_unmarked(%{{.*}}, %[[C]])

// REMARK: tag rewritten to spsc_inline
// REMARK: tag rewritten to spsc{{$}}
// REMARK: every pop borrows, but this element type cannot be held inline; plain spsc
// REMARK: tag rewritten to spsc{{$}}
