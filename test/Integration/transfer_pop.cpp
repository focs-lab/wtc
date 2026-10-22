// The transfer form of pop end to end: a move-only element pushed by
// moving and popped into the consumer's object. The compiler proves the
// queue spsc; the pop keeps nothing of the queue's, so the queue is lent
// inline (the element is trivially destructible), and the consumer's step,
// declared lock-free, passes: on the ring a transfer allocates nothing and
// waits for nothing. On the general-purpose queue the same step fails,
// since that pop frees the segments it moves past. Every element arrives
// once and in order, under ThreadSanitizer and WTC_CHECK as well.
//
// RUN: %wtc_cc %examples/transfer_pipeline.cpp -o %t.exe 2>&1 | FileCheck %s --check-prefix=PASSES
// RUN: %t.exe | FileCheck %s
// RUN: %wtc_cc -O2 %examples/transfer_pipeline.cpp -o %t.o2.exe 2>&1 | FileCheck %s --check-prefix=PASSES
// RUN: %t.o2.exe | FileCheck %s
// RUN: %wtc_cc -DWTC_CHECK %examples/transfer_pipeline.cpp -o %t.check.exe
// RUN: %t.check.exe | FileCheck %s
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %examples/transfer_pipeline.cpp -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefix=PASSES %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,CHECK %}
// RUN: not %wtc_cc --no-specialize %examples/transfer_pipeline.cpp -o %t.generic.exe 2>&1 | FileCheck %s --check-prefix=GENERIC
//
// PASSES: queue @q: spsc (producers: 1, consumers: 1)
// PASSES: queue @q: every pop only borrows its element (1 pop)
// PASSES: tag rewritten to spsc_inline
// PASSES: remark: wtc: consume_one(token&) is declared lock-free: nothing it reaches blocks
// TSAN-NOT: ThreadSanitizer
// CHECK: tag spsc_inline in_order 1 sum 59999700000
//
// GENERIC: error: wtc: consume_one(token&) is declared lock-free but can block
// GENERIC: note: which waits for a push that has taken the cell it reads to fill it, and frees the segments it moves past
