// What blocks, one function declared lock-free per reason, and what does
// not: atomics, memcpy, arithmetic, and calls into functions that only do
// those. The chain of calls leading to the blocking operation is named.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s

#include "mutex.hpp"
#include "progress.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

wtc::mutex m;
std::atomic<long> counter{0};
long plain = 0;
void (*hook)() = nullptr;
void opaque();
pthread_mutex_t raw = PTHREAD_MUTEX_INITIALIZER;

// CHECK-DAG: error: wtc: locked() is declared lock-free but can block
// CHECK-DAG: note: which takes a mutex
WTC_LOCK_FREE void locked() { m.lock(); ++plain; m.unlock(); }

// CHECK-DAG: error: wtc: logs() is declared lock-free but can block
// CHECK-DAG: note: calls log_value(long)
// CHECK-DAG: note: which does input or output (printf)
void log_value(long v) { std::printf("%ld\n", v); }
WTC_LOCK_FREE void logs() { log_value(plain); }

// CHECK-DAG: error: wtc: allocates() is declared lock-free but can block
// CHECK-DAG: note: which calls the allocator (operator new(unsigned long))
WTC_LOCK_FREE void allocates() { plain += reinterpret_cast<long>(new long(1)); }

// CHECK-DAG: error: wtc: hooked() is declared lock-free but can block
// CHECK-DAG: note: which makes an indirect call whose target is not known here
WTC_LOCK_FREE void hooked() { hook(); }

// An indirect call whose targets are known is judged by them: the step
// here is a local pointer set once, to a function that only counts.
// CHECK-DAG: remark: wtc: known_target() is declared lock-free: nothing it reaches blocks
void count_one() { counter.fetch_add(1, std::memory_order_relaxed); }
WTC_LOCK_FREE void known_target() { void (*step)() = count_one; step(); }

// ... and when the one target can block, the chain goes through the call.
// CHECK-DAG: error: wtc: known_blocking_target() is declared lock-free but can block
// CHECK-DAG: note: calls log_plain() through a pointer
void log_plain() { log_value(plain); }
WTC_LOCK_FREE void known_blocking_target() { void (*step)() = log_plain; step(); }

// CHECK-DAG: error: wtc: waits() is declared lock-free but can block
// CHECK-DAG: note: which waits in the threading library (pthread_mutex_lock)
WTC_LOCK_FREE void waits() { pthread_mutex_lock(&raw); ++plain; pthread_mutex_unlock(&raw); }

// CHECK-DAG: error: wtc: sleeps() is declared lock-free but can block
// CHECK-DAG: note: which sleeps or yields (usleep)
WTC_LOCK_FREE void sleeps() { usleep(1); }

// A throw is spelled as operations, not as a call into the runtime, and
// blocks all the same.
// CHECK-DAG: error: wtc: throws() is declared lock-free but can block
// CHECK-DAG: note: which throws: the C++ runtime allocates the exception and unwinds the stack
WTC_LOCK_FREE void throws() { if (plain < 0) throw plain; }

// CHECK-DAG: error: wtc: unseen() is declared lock-free but can block
// CHECK-DAG: note: which calls opaque(), whose body is not visible here, so it is counted as blocking
WTC_LOCK_FREE void unseen() { opaque(); }

// CHECK-DAG: error: wtc: recursive(int) is declared lock-free but can block
// CHECK-DAG: note: calls deeper(int)
void recursive(int n);
void deeper(int n) { if (n > 3) log_value(n); else recursive(n + 1); }
WTC_LOCK_FREE void recursive(int n) { deeper(n); }

// CHECK-DAG: remark: wtc: clean() is declared lock-free: nothing it reaches blocks
long helper(long x) { return x * 3 + counter.load(std::memory_order_relaxed); }
WTC_LOCK_FREE void clean() {
    long buf[4] = {1, 2, 3, 4}, out[4];
    std::memcpy(out, buf, sizeof buf);
    counter.fetch_add(helper(out[2]), std::memory_order_relaxed);
}

// Every blocking site of the declared function is named, each with its
// own chain, not only the first: real code usually has several.
// CHECK-DAG: error: wtc: two_sites() is declared lock-free but can block
// CHECK-DAG: note: which waits in the threading library (pthread_mutex_trylock)
// CHECK-DAG: note: which does input or output (fputs)
WTC_LOCK_FREE void two_sites() {
    if (pthread_mutex_trylock(&raw) == 0) { ++plain; pthread_mutex_unlock(&raw); }
    std::fputs("x\n", stderr);
}

// Not declared: nothing is said about it, whatever it does.
// CHECK-NOT: undeclared
void undeclared() { std::printf("x\n"); }

int main() {
    locked(); logs(); allocates(); hooked(); unseen(); recursive(0); clean(); undeclared();
    throws(); known_target(); known_blocking_target(); waits(); sleeps();
    two_sites();
}
