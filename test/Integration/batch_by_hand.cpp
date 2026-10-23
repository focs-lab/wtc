// A run of deferred pushes written by hand goes through the whole pipeline
// -- lifted to the batch operations, lowered back to the shim's calls --
// and delivers every element once and in order on all three
// implementations. Tag 0 is not a declaration, so the compiler is kept from
// choosing for it; left to choose, it picks inline storage, since the
// queue is spsc and its consumer only borrows.
//
// RUN: %wtc_cc --no-specialize -DTAG=0 %examples/batch_by_hand.cpp -o %t.0.exe && %t.0.exe | FileCheck %s --check-prefix=T0
// RUN: %wtc_cc -DTAG=1 %examples/batch_by_hand.cpp -o %t.1.exe && %t.1.exe | FileCheck %s --check-prefix=T1
// RUN: %wtc_cc -DTAG=2 %examples/batch_by_hand.cpp -o %t.2.exe && %t.2.exe | FileCheck %s --check-prefix=T2
// RUN: %wtc_cc %examples/batch_by_hand.cpp -o %t.c.exe && %t.c.exe | FileCheck %s --check-prefix=T2
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g --no-specialize -DTAG=0 %examples/batch_by_hand.cpp -o %t.tsan0.exe && env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan0.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,T0 %}
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g -DTAG=1 %examples/batch_by_hand.cpp -o %t.tsan1.exe && env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan1.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,T1 %}
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g -DTAG=2 %examples/batch_by_hand.cpp -o %t.tsan2.exe && env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan2.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,T2 %}
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %examples/batch_by_hand.cpp -o %t.tsanc.exe && env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsanc.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,T2 %}
//
// TSAN-NOT: ThreadSanitizer
// T0: tag 0 in_order 1
// T1: tag 1 in_order 1
// T2: tag 2 in_order 1
