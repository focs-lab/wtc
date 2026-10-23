// A WTC_BATCH loop of pushes by rvalue, of an element that cannot be
// copied: the batching pass must pick the moving deferred push, and the
// program must deliver every element once and in order. The copying
// overload's body for such a type is a call to abort, which is what a
// wrong choice would hit.
//
// RUN: %wtc_cc %examples/batch_move.cpp -o %t.exe 2>&1 | FileCheck %s --check-prefix=BATCHED
// RUN: %t.exe | FileCheck %s
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %examples/batch_move.cpp -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefix=BATCHED %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,CHECK %}
//
// BATCHED: pushes in this loop batched
// TSAN-NOT: ThreadSanitizer
// CHECK: in_order 1 got 100000
