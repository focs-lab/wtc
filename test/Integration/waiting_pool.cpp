// The waiting pop end to end: a pool whose workers sleep in pop_wait
// between rounds of tasks and return once the queue is closed and empty.
// The constructor's second argument is the wait policy; the compiler
// rewrites the first, the topology, to spmc. Every task runs exactly once
// under every policy, under ThreadSanitizer, and with WTC_CHECK, which
// aborts if a second thread ever pushes.
//
// RUN: %wtc_cc %examples/waiting_pool.cpp -o %t.exe 2>&1 | FileCheck %s
// RUN: %t.exe | FileCheck %s --check-prefix=PARK
// RUN: %wtc_cc -DWAIT_POLICY=0 %examples/waiting_pool.cpp -o %t.spin.exe
// RUN: %t.spin.exe | FileCheck %s --check-prefix=SPIN
// RUN: %wtc_cc -DWAIT_POLICY=2 -DWTC_CHECK %examples/waiting_pool.cpp -o %t.check.exe 2>&1 | FileCheck %s
// RUN: %t.check.exe | FileCheck %s --check-prefix=STP
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %examples/waiting_pool.cpp -o %t.tsan.exe %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,PARK %}
//
// CHECK: queue @tasks: spmc (producers: 1, consumers: many)
// CHECK: specialised: constructor tag rewritten to spmc
// TSAN-NOT: ThreadSanitizer
// PARK: tag 4 policy 1 ok 10000
// SPIN: tag 4 policy 0 ok 10000
// STP: tag 4 policy 2 ok 10000
