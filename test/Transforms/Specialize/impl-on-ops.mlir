// RUN: wtc-opt %s --wtc-specialize 2>/dev/null | FileCheck %s
// RUN: wtc-opt %s --wtc-specialize=rewrite=false 2>/dev/null | FileCheck %s --check-prefix=AS-IS

// Every queue operation records the implementation its queue runs, read
// back from the tag its constructor passes: @s is proved spsc and its tag
// is rewritten, @d declares spsc itself, @g stays general-purpose, and the
// pop in @either may act on two queues, so it is general-purpose too.
// With rewrite=false nothing is rewritten and only the declared tag counts.

!s32i = !cir.int<s, 32>
!queue_i32 = !cir.struct<"wtc::queue<int>" {data !cir.ptr<!s32i>}>
module {
  cir.func private @ctor(!cir.ptr<!queue_i32>, !s32i) [#cir.annotation<"wtc_queue_construction">]
  cir.global external @s = #cir.zero : !queue_i32 {wtc.queue_id = 0 : i64, wtc.topology = #wtc.topology<spsc>}
  cir.global external @d = #cir.zero : !queue_i32 {wtc.queue_id = 1 : i64, wtc.topology = #wtc.topology<spsc>}
  cir.global external @g = #cir.zero : !queue_i32 {wtc.queue_id = 2 : i64, wtc.topology = #wtc.topology<mpmc>}
  cir.global "private" external @v : !s32i

  cir.func @init() {
    %zero = cir.const #cir.int<0> : !s32i
    %one = cir.const #cir.int<1> : !s32i
    %s = cir.get_global @s : !cir.ptr<!queue_i32>
    cir.call @ctor(%s, %zero) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    %d = cir.get_global @d : !cir.ptr<!queue_i32>
    cir.call @ctor(%d, %one) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    %g = cir.get_global @g : !cir.ptr<!queue_i32>
    cir.call @ctor(%g, %zero) : (!cir.ptr<!queue_i32>, !s32i) -> ()
    cir.return
  }

  cir.func @use() {
    %v = cir.get_global @v : !cir.ptr<!s32i>
    %s = cir.get_global @s : !cir.ptr<!queue_i32>
    %hs = builtin.unrealized_conversion_cast %s : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    "wtc.queue_push"(%hs, %v) {queue_id = 0 : i64} : (!wtc.queue<!queue_i32>, !cir.ptr<!s32i>) -> ()
    %d = cir.get_global @d : !cir.ptr<!queue_i32>
    %hd = builtin.unrealized_conversion_cast %d : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    "wtc.queue_push"(%hd, %v) {queue_id = 1 : i64} : (!wtc.queue<!queue_i32>, !cir.ptr<!s32i>) -> ()
    %g = cir.get_global @g : !cir.ptr<!queue_i32>
    %hg = builtin.unrealized_conversion_cast %g : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    "wtc.queue_push"(%hg, %v) {queue_id = 2 : i64} : (!wtc.queue<!queue_i32>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @either(%q: !cir.ptr<!queue_i32>) {
    %h = builtin.unrealized_conversion_cast %q : !cir.ptr<!queue_i32> to !wtc.queue<!queue_i32>
    %x = "wtc.queue_pop"(%h) : (!wtc.queue<!queue_i32>) -> !cir.ptr<!s32i>
    cir.return
  }
}

// CHECK-LABEL: cir.func @use
// CHECK: "wtc.queue_push"({{.*}}) <{impl = #wtc.impl<spsc>, queue_id = 0
// CHECK: "wtc.queue_push"({{.*}}) <{impl = #wtc.impl<spsc>, queue_id = 1
// CHECK: "wtc.queue_push"({{.*}}) <{impl = #wtc.impl<generic>, queue_id = 2
// CHECK-LABEL: cir.func @either
// CHECK: "wtc.queue_pop"({{.*}}) <{impl = #wtc.impl<generic>}>

// AS-IS-LABEL: cir.func @use
// AS-IS: "wtc.queue_push"({{.*}}) <{impl = #wtc.impl<generic>, queue_id = 0
// AS-IS: "wtc.queue_push"({{.*}}) <{impl = #wtc.impl<spsc>, queue_id = 1
// AS-IS: "wtc.queue_push"({{.*}}) <{impl = #wtc.impl<generic>, queue_id = 2
