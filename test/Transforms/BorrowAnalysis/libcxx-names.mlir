// RUN: wtc-opt %s --wtc-borrow-analysis 2>&1 >/dev/null | FileCheck %s

// The members of shared_ptr are recognised by their demangled names, so
// libc++'s spellings -- the std::__1 namespace, ABI tags on the names, a
// complete-object destructor -- count the same as libstdc++'s. libc++ is not
// installed where the suite runs, so its output is written out here.

!s64i = !cir.int<s, 64>
!rec = !cir.struct<"record" {data !s64i}>
!sp = !cir.struct<"std::__1::shared_ptr<record>" {data !cir.ptr<!rec>, data !cir.ptr<!s64i>}>
!q = !cir.struct<"wtc::queue<record>" {data !s64i}>

module {
  cir.global external @q = #cir.zero : !q {wtc.queue_id = 0 : i64, wtc.topology = #wtc.topology<spsc>}
  cir.global external @r = #cir.zero : !q {wtc.queue_id = 1 : i64, wtc.topology = #wtc.topology<spsc>}
  cir.global external @sum = #cir.int<0> : !s64i

  cir.func private @_ZN3wtc5queueI6recordE7try_popEv(!cir.ptr<!sp> {llvm.sret = !sp}, !cir.ptr<!q>) [#cir.annotation<"wtc_queue_try_pop">]
  cir.func private @_ZNKSt3__110shared_ptrI6recordEcvbB8ne200100Ev(!cir.ptr<!sp>) -> !cir.bool
  cir.func private @_ZNKSt3__110shared_ptrI6recordEptB8ne200100Ev(!cir.ptr<!sp>) -> !cir.ptr<!rec>
  cir.func private @_ZNSt3__1neB8ne200100I6recordEEbRKNS_10shared_ptrIT_EEDn(!cir.ptr<!sp>, !cir.ptr<!s64i>) -> !cir.bool
  cir.func private @_ZNKSt3__110shared_ptrI6recordE9use_countB8ne200100Ev(!cir.ptr<!sp>) -> !s64i
  cir.func private @_ZNSt3__110shared_ptrI6recordED1Ev(!cir.ptr<!sp>)

  cir.func @consume_q() {
    cir.scope {
      %p = cir.alloca "p" align(8) init : !cir.ptr<!sp>
      %g = cir.get_global @q : !cir.ptr<!q>
      %h = builtin.unrealized_conversion_cast %g : !cir.ptr<!q> to !wtc.queue<!q>
      "wtc.queue_pop"(%h, %p) {queue_id = 0 : i64} : (!wtc.queue<!q>, !cir.ptr<!sp>) -> ()
      %nil = cir.const #cir.ptr<null> : !cir.ptr<!s64i>
      %ne = cir.call @_ZNSt3__1neB8ne200100I6recordEEbRKNS_10shared_ptrIT_EEDn(%p, %nil) : (!cir.ptr<!sp>, !cir.ptr<!s64i>) -> !cir.bool
      %ok = cir.call @_ZNKSt3__110shared_ptrI6recordEcvbB8ne200100Ev(%p) : (!cir.ptr<!sp>) -> !cir.bool
      %e = cir.call @_ZNKSt3__110shared_ptrI6recordEptB8ne200100Ev(%p) : (!cir.ptr<!sp>) -> !cir.ptr<!rec>
      %f = cir.get_member %e[0] {name = "seq"} : !cir.ptr<!rec> -> !cir.ptr<!s64i>
      %v = cir.load %f : !cir.ptr<!s64i>, !s64i
      %s = cir.get_global @sum : !cir.ptr<!s64i>
      cir.store %v, %s : !s64i, !cir.ptr<!s64i>
      cir.call @_ZNSt3__110shared_ptrI6recordED1Ev(%p) : (!cir.ptr<!sp>) -> ()
    }
    cir.return
  }

  cir.func @consume_r() {
    cir.scope {
      %p = cir.alloca "p" align(8) init : !cir.ptr<!sp>
      %g = cir.get_global @r : !cir.ptr<!q>
      %h = builtin.unrealized_conversion_cast %g : !cir.ptr<!q> to !wtc.queue<!q>
      "wtc.queue_pop"(%h, %p) {queue_id = 1 : i64} : (!wtc.queue<!q>, !cir.ptr<!sp>) -> ()
      %n = cir.call @_ZNKSt3__110shared_ptrI6recordE9use_countB8ne200100Ev(%p) : (!cir.ptr<!sp>) -> !s64i
      cir.call @_ZNSt3__110shared_ptrI6recordED1Ev(%p) : (!cir.ptr<!sp>) -> ()
    }
    cir.return
  }
}

// CHECK-DAG: queue @q: every pop only borrows its element (1 pop)
// CHECK-DAG: queue @r: not borrowed: the popped shared_ptr is passed to std::__1::shared_ptr<record>::use_count[abi:ne200100]() const
