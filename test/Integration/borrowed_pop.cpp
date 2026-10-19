// The whole pipeline, unmodified source: the compiler proves the queue spsc
// and its consumer a borrower, chooses inline storage, and the program sees
// every element once and in order. At -O2 as well as -O0, since the
// consumer's slot takes a different shape there. Under WTC_CHECK the guard
// that catches a kept element must stay silent.
//
// RUN: %wtc_cc %examples/inline_pipeline.cpp -o %t.exe 2>&1 | FileCheck %s --check-prefix=PASSES
// RUN: %t.exe | FileCheck %s
// RUN: %wtc_cc -O2 %examples/inline_pipeline.cpp -o %t.o2.exe 2>&1 | FileCheck %s --check-prefix=PASSES
// RUN: %t.o2.exe | FileCheck %s
// RUN: %wtc_cc -DWTC_CHECK %examples/inline_pipeline.cpp -o %t.check.exe
// RUN: %t.check.exe | FileCheck %s
//
// PASSES: queue @q: every pop only borrows its element (1 pop)
// PASSES: tag rewritten to spsc_inline
// CHECK: tag spsc_inline in_order 1 sum 59999700000
