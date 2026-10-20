// Critical sections held as one, through the whole pipeline: two threads
// add to one counter in loops the compiler batches, and the sum is exact.
//
// RUN: %wtc_cc %examples/lock_loop.cpp -o %t.exe 2>&1 | FileCheck %s --check-prefix=BUILD
// RUN: %t.exe | FileCheck %s
// RUN: %wtc_cc -DPLAIN %examples/lock_loop.cpp -o %t.plain.exe 2>&1 | FileCheck %s --allow-empty --check-prefix=PLAIN
// RUN: %t.plain.exe | FileCheck %s
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g -DPLAIN %examples/lock_loop.cpp -o %t.tsan.plain.exe 2>&1 | FileCheck %s --allow-empty --check-prefix=PLAIN %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.plain.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,CHECK %}
//
// BUILD: remark: wtc: critical sections in this loop batched
// PLAIN-NOT: critical sections in this loop batched
// TSAN-NOT: ThreadSanitizer
// CHECK: total 40001000006 ok 1
