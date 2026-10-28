// What holding a mutex across critical sections takes from the shared
// answer to "may this wait" (Transforms/Blocking.h), which the progress
// check gives too: an indirect call whose targets are all known is followed
// into them, and prefetches and the allocator (realloc among it) cannot
// wait for another thread. A pointer parameter is resolved only for a
// function no other translation unit can call, and every target counts.
// The allocator is the C library's and the replaceable operator new and
// delete, not a placement form of the program's, which may take a lock;
// DPDK's ring and mempool constructors take the EAL's tail-queue lock.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s

#include "mutex.hpp"
#include "progress.hpp"

#include <cstdlib>
#include <pthread.h>

wtc::mutex m;
long total;
int *buf;
static void add(long x) noexcept { total += x; }
static void add_twice(long x) noexcept { total += 2 * x; }
pthread_mutex_t raw = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
struct Pool;
void *operator new(std::size_t size, Pool &pool) noexcept;  // takes the pool's lock
extern Pool pool;
struct Node { long v; };
struct rte_ring;
extern "C" rte_ring *rte_ring_create(const char *name, unsigned count, int socket, unsigned flags) noexcept;
struct rte_mempool;
extern "C" rte_mempool *rte_pktmbuf_pool_create(const char *name, unsigned n, unsigned cache_size,
                                                unsigned short priv_size, unsigned short data_room_size,
                                                int socket_id) noexcept;
static void wait_ready(long) noexcept { pthread_mutex_lock(&raw); pthread_cond_wait(&ready, &raw); pthread_mutex_unlock(&raw); }

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched
WTC_BATCH static void through(int n, void (*f)(long) noexcept) { for (int i = 0; i < n; ++i) { m.lock(); f(i); m.unlock(); } }
void calls_through(int n) { through(n, add); through(n, add_twice); }
// Another translation unit may call this one with a pointer to code that
// waits for a thread that needs m.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it makes an indirect call
WTC_BATCH void through_extern(int n, void (*f)(long) noexcept) { for (int i = 0; i < n; ++i) { m.lock(); f(i); m.unlock(); } }
void calls_through_extern(int n) { through_extern(n, add); }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls wait_ready(long), where it calls into the threading library (pthread_mutex_lock)
WTC_BATCH static void through_mixed(int n, void (*f)(long) noexcept) { for (int i = 0; i < n; ++i) { m.lock(); f(i); m.unlock(); } }
void calls_through_mixed(int n) { through_mixed(n, add); through_mixed(n, wait_ready); }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched
WTC_BATCH void prefetches(int n, const long *p) { for (int i = 0; i < n; ++i) { asm volatile("prefetcht0 %0" : : "m"(p[i + 8])); m.lock(); total += p[i]; m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop batched
WTC_BATCH void grows(int n) { for (int i = 0; i < n; ++i) { m.lock(); buf = static_cast<int *>(std::realloc(buf, (i + 1) * sizeof(int))); m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls operator new(unsigned long, Pool&), which may wait for another thread
WTC_BATCH void pooled(int n) { for (int i = 0; i < n; ++i) { Node *x = new (pool) Node; m.lock(); total += x->v; m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls rte_ring_create, which may wait for another thread
WTC_BATCH void rings(int n) { for (int i = 0; i < n; ++i) { m.lock(); total += reinterpret_cast<long>(rte_ring_create("r", 64, 0, 0)); m.unlock(); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: critical sections in this loop not batched: it calls rte_pktmbuf_pool_create, which may wait for another thread
WTC_BATCH void pools(int n) { for (int i = 0; i < n; ++i) { m.lock(); total += reinterpret_cast<long>(rte_pktmbuf_pool_create("p", 1023, 32, 0, 2048, 0)); m.unlock(); } }
