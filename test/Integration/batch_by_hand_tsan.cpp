// REQUIRES: tsan
// RUN: %tsan_cxx -std=c++17 -O1 -g -fsanitize=thread -I%shim -DTAG=0 %examples/batch_by_hand.cpp -o %t.0.exe -pthread
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.0.exe 2>&1 | FileCheck %s --check-prefix=T0
// RUN: %tsan_cxx -std=c++17 -O1 -g -fsanitize=thread -I%shim -DTAG=2 %examples/batch_by_hand.cpp -o %t.2.exe -pthread
// RUN: env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.2.exe 2>&1 | FileCheck %s --check-prefix=T2
//
// T0-NOT: ThreadSanitizer
// T0: tag 0 in_order 1
// T2-NOT: ThreadSanitizer
// T2: tag 2 in_order 1
