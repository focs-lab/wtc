// What the batching of pushes takes from the shared answer to "may this
// wait" (Transforms/Blocking.h), which the progress check gives too: an
// indirect call whose targets are all known is followed into them rather
// than refused, and inline assembly made only of prefetches is no
// obstacle, while a compiler barrier, the spin-wait hint and a prefetch
// with a memory clobber are. A pointer parameter is resolved only for a
// function no other translation unit can call, and every target counts. A
// function without a body here that is not known not to wait is refused,
// nothrow or not.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-pushes -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir

#include "progress.hpp"
#include "queue.hpp"

#include <cstdio>
#include <pthread.h>
#include <sys/sem.h>

wtc::queue<int> q;
long seen;
static void count(int x) noexcept { seen += x; }
static void count_twice(int x) noexcept { seen += 2 * x; }
pthread_mutex_t raw = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static void wait_ready(int) noexcept { pthread_mutex_lock(&raw); pthread_cond_wait(&ready, &raw); pthread_mutex_unlock(&raw); }
void report(int) noexcept;
int ack;
sembuf down = {0, -1, 0};

// Through a pointer that is always count or count_twice: both are seen.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop batched
WTC_BATCH static void through(int n, void (*f)(int) noexcept) { for (int i = 0; i < n; ++i) { q.push(i); f(i); } }
void calls_through(int n) { through(n, count); through(n, count_twice); }

// Not when another translation unit may call it with any other pointer...
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it makes an indirect call
WTC_BATCH void through_extern(int n, void (*f)(int) noexcept) { for (int i = 0; i < n; ++i) { q.push(i); f(i); } }
void calls_through_extern(int n) { through_extern(n, count); }
// ... and not when one of the targets waits.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls wait_ready(int), where it calls into the threading library (pthread_mutex_lock)
WTC_BATCH static void through_mixed(int n, void (*f)(int) noexcept) { for (int i = 0; i < n; ++i) { q.push(i); f(i); } }
void calls_through_mixed(int n) { through_mixed(n, count); through_mixed(n, wait_ready); }

// A consumer that raises the semaphore once per pop would never see the
// pushes held back while this loop waits for it.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls semop, which may wait for another thread
WTC_BATCH void handshakes(int n) { for (int i = 0; i < n; ++i) { q.push(i); semop(ack, &down, 1); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls report(int), which may wait for another thread
WTC_BATCH void reports(int n) { for (int i = 0; i < n; ++i) { q.push(i); report(i); } }
// A write to a pipe or a socket waits for its reader, which may be the
// consumer: refused even declared nothrow. As the C library declares it,
// it is refused before that as a call that may throw, as printf is.
extern "C" long write(int fd, const void *buf, unsigned long n) noexcept;
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls write, which may wait for another thread
WTC_BATCH void writes(int n, int fd) { for (int i = 0; i < n; ++i) { q.push(i); write(fd, &i, sizeof i); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls printf, which may throw
WTC_BATCH void prints(int n) { for (int i = 0; i < n; ++i) { q.push(i); std::printf("%d\n", i); } }

// Sleeping waits for no thread: accepted, declared nothrow. (The C
// library's sleeps are not declared so, and are refused as calls that may
// throw.)
extern "C" unsigned sleep(unsigned seconds) noexcept;
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop batched
WTC_BATCH void naps(int n) { for (int i = 0; i < n; ++i) { q.push(i); sleep(0); } }

// A weak definition may be replaced at link time by another translation
// unit's, which may wait: its body here is not followed.
__attribute__((weak)) void on_item(int) noexcept {}
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it calls on_item(int), which may wait for another thread
WTC_BATCH void hooks(int n) { for (int i = 0; i < n; ++i) { q.push(i); on_item(i); } }

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop batched
WTC_BATCH void prefetches(int n, const int *p) { for (int i = 0; i < n; ++i) { asm volatile("prefetcht0 %0" : : "m"(p[i + 8])); q.push(p[i]); } }

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it synchronises (cir.asm)
WTC_BATCH void barrier(int n) { for (int i = 0; i < n; ++i) { q.push(i); asm volatile("" ::: "memory"); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it synchronises (cir.asm)
WTC_BATCH void clobbers(int n, const int *p) { for (int i = 0; i < n; ++i) { asm volatile("prefetcht0 %0" : : "m"(p[i + 8]) : "memory"); q.push(p[i]); } }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: pushes in this loop not batched: it synchronises (cir.asm)
WTC_BATCH void pauses(int n) { for (int i = 0; i < n; ++i) { q.push(i); asm volatile("pause"); } }

// IR-LABEL: cir.func {{.*}}@_ZL7throughiPDoFviE(
// IR: "wtc.queue_push_deferred"
// IR-LABEL: cir.func {{.*}}@_Z10prefetchesiPKi(
// IR: "wtc.queue_push_deferred"
// IR-LABEL: cir.func {{.*}}@_Z7barrieri(
// IR-NOT: wtc.queue_push_deferred
// IR: cir.return
// IR-LABEL: cir.func {{.*}}@_Z6pausesi(
// IR-NOT: wtc.queue_push_deferred
// IR: cir.return
