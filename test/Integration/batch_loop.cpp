// A loop of pushes in a WTC_BATCH function, through the whole pipeline: the
// pushes are batched, and every element arrives once and in order with the
// implementation the compiler chooses (inline, since the queue is spsc and
// its consumer borrows), with the general-purpose one, and with a declared
// spsc.
//
// RUN: %wtc_cc %examples/batch_loop.cpp -o %t.exe 2>&1 | FileCheck %s --check-prefix=BUILD
// RUN: %t.exe | FileCheck %s --check-prefix=T2
// RUN: %wtc_cc --no-specialize %examples/batch_loop.cpp -o %t.generic.exe && %t.generic.exe | FileCheck %s --check-prefix=T0
// RUN: %wtc_cc -DTAG=1 %examples/batch_loop.cpp -o %t.1.exe && %t.1.exe | FileCheck %s --check-prefix=T1
//
// BUILD: remark: wtc: pushes in this loop batched
// BUILD: tag rewritten to spsc_inline
// T2: tag 2 in_order 1
// T0: tag 0 in_order 1
// T1: tag 1 in_order 1
