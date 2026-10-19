// The inline queue under ThreadSanitizer: a producer and a consumer that
// borrows every element in place must not race.
//
// REQUIRES: tsan
// RUN: %tsan_cxx -std=c++17 -O1 -g -fsanitize=thread -I%shim -DDECLARE_INLINE %examples/inline_pipeline.cpp -o %t.exe -pthread
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.exe 2>&1 | FileCheck %s
//
// CHECK-NOT: ThreadSanitizer
// CHECK: tag spsc_inline in_order 1 sum 59999700000
