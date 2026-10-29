// RUN: wtc-opt %s --queue-ownership-analysis 2>&1 >/dev/null | FileCheck %s

// The same, spelled with a while loop rather than a for loop. The test
// exists because the rule is written against the loop interface rather than
// against one operation, and an interface claim is worth one test per
// implementation of it.

!s32i = !cir.int<s, 32>
module {
  cir.global "private" external @q = #cir.int<0> : !s32i
  cir.global "private" external @v = #cir.int<0> : !s32i

  cir.func @spawn(%fn: !cir.ptr<!cir.func<()>>) [#cir.annotation<"wtc_thread_spawn">] {
    cir.return
  }

  cir.func @consumer() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    %popped = "wtc.queue_pop"(%qcast) : (!wtc.queue<!s32i>) -> !cir.ptr<!s32i>
    cir.return
  }

  cir.func @producer() {
    %queue = cir.get_global @q : !cir.ptr<!s32i>
    %value = cir.get_global @v : !cir.ptr<!s32i>
    %qcast = builtin.unrealized_conversion_cast %queue : !cir.ptr<!s32i> to !wtc.queue<!s32i>
    "wtc.queue_push"(%qcast, %value) : (!wtc.queue<!s32i>, !cir.ptr<!s32i>) -> ()
    cir.return
  }

  cir.func @main() {
    %p = cir.get_global @producer : !cir.ptr<!cir.func<()>>
    cir.call @spawn(%p) : (!cir.ptr<!cir.func<()>>) -> ()
    cir.scope {
      cir.while {
        %t = cir.const #cir.bool<true> : !cir.bool
        cir.condition(%t)
      } do {
        %c = cir.get_global @consumer : !cir.ptr<!cir.func<()>>
        cir.call @spawn(%c) : (!cir.ptr<!cir.func<()>>) -> ()
        cir.yield
      }
    }
    cir.return
  }
}

// CHECK: queue @q: spmc (producers: 1, consumers: many)
