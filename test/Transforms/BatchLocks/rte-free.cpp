// DPDK's rte_free is the allocator, which cannot wait for a mutex the
// program holds, but its declaration is not nothrow, and the ClangIR in use
// marks a call nothrow only when the declaration is: from C++, under
// -fno-exceptions as well, a call to it may throw, and the loop is left
// alone. Declared nothrow, it is accepted.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --check-prefix=THROWS
// RUN: %clangxx -std=c++17 -fno-exceptions -I%shim -fclangir -emit-cir %s -o %t.noexc.cir
// RUN: wtc-opt %t.noexc.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --check-prefix=THROWS
// RUN: %clangxx -std=c++17 -DNOTHROW -I%shim -fclangir -emit-cir %s -o %t.nothrow.cir
// RUN: wtc-opt %t.nothrow.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOTHROW

#include "mutex.hpp"
#include "progress.hpp"

#ifdef NOTHROW
extern "C" void rte_free(void *ptr) noexcept;
#else
extern "C" void rte_free(void *ptr);  // as rte_malloc.h declares it
#endif

wtc::mutex m;
void *slots[64];

// THROWS: [[@LINE+2]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls rte_free, which may throw
// NOTHROW: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched
WTC_BATCH void drain(int n) { for (int i = 0; i < n; ++i) { m.lock(); rte_free(slots[i % 64]); slots[i % 64] = nullptr; m.unlock(); } }
