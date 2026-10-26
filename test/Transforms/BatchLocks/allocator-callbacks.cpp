// The allocator is accepted as a leaf: it waits for no thread of the
// program and runs none of its code. A program that registers code for it
// to run gives that up: neither batching pass accepts the allocator, and
// no loop's pushes are batched, since a push allocates when its queue
// grows. Each registration is tried alone: a new-handler, which operator
// new calls when memory runs short; a DPDK memory event callback and an
// allocation validator, run inside rte_malloc and rte_free; one of glibc's
// old allocation hooks.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DNEW_HANDLER %s -o %t.nh.cir
// RUN: wtc-opt %t.nh.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOCKS -DREG='std::set_new_handler(void (*)())'
// RUN: wtc-opt %t.nh.cir --lift-cir-to-wtc --wtc-batch-pushes -o /dev/null 2>&1 | FileCheck %s --check-prefix=PUSHES -DREG='std::set_new_handler(void (*)())'
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DMEM_EVENT %s -o %t.ev.cir
// RUN: wtc-opt %t.ev.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOCKS -DREG=rte_mem_event_callback_register
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DVALIDATOR %s -o %t.va.cir
// RUN: wtc-opt %t.va.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOCKS -DREG=rte_mem_alloc_validator_register
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DHOOK %s -o %t.hk.cir
// RUN: wtc-opt %t.hk.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --check-prefix=LOCKS -DREG=__malloc_hook
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.none.cir
// RUN: wtc-opt %t.none.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --check-prefix=NONE

#include "mutex.hpp"
#include "progress.hpp"
#include "queue.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>

wtc::mutex m;
wtc::queue<int> q;
void *slots[64];

#if defined(NEW_HANDLER)
void wait_for_memory();
void install() { std::set_new_handler(wait_for_memory); }
#elif defined(MEM_EVENT)
extern "C" int rte_mem_event_callback_register(const char *name, void (*cb)(int, const void *, std::size_t, void *),
                                               void *arg);
void on_event(int, const void *, std::size_t, void *);
void install() { rte_mem_event_callback_register("pool", on_event, nullptr); }
#elif defined(VALIDATOR)
extern "C" int rte_mem_alloc_validator_register(const char *name, int (*cb)(int, std::size_t, std::size_t),
                                                int socket, std::size_t limit);
int validate(int, std::size_t, std::size_t);
void install() { rte_mem_alloc_validator_register("pool", validate, 0, 1 << 20); }
#elif defined(HOOK)
extern "C" void *(*__malloc_hook)(std::size_t, const void *);  // glibc before 2.34
void *my_malloc(std::size_t, const void *);
void install() { __malloc_hook = my_malloc; }
#endif

// LOCKS: [[@LINE+2]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls malloc, and the program registers code for the allocator to run ([[REG]])
// NONE: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched
WTC_BATCH void fill(int n) { for (int i = 0; i < n; ++i) { m.lock(); slots[i % 64] = std::malloc(16); m.unlock(); } }
// PUSHES: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: a push may allocate, and the program registers code for the allocator to run ([[REG]])
WTC_BATCH void produce(int n) { for (int i = 0; i < n; ++i) q.push(i); }
