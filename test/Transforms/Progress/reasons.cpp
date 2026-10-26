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

// A pointer parameter is resolved from the calls seen here only when no
// other translation unit can call the function.
// CHECK-DAG: remark: wtc: through_static(void (*)()) is declared lock-free: nothing it reaches blocks
WTC_LOCK_FREE static void through_static(void (*step)()) { step(); }
// CHECK-DAG: error: wtc: through_extern(void (*)()) is declared lock-free but can block
WTC_LOCK_FREE void through_extern(void (*step)()) { step(); }
void calls_through() { through_static(count_one); through_extern(count_one); }

// CHECK-DAG: error: wtc: waits() is declared lock-free but can block
// CHECK-DAG: note: which waits in the threading library (pthread_mutex_lock)
WTC_LOCK_FREE void waits() { pthread_mutex_lock(&raw); ++plain; pthread_mutex_unlock(&raw); }

// CHECK-DAG: error: wtc: sleeps() is declared lock-free but can block
// CHECK-DAG: note: which sleeps or yields (usleep)
WTC_LOCK_FREE void sleeps() { usleep(1); }

// A function-local static initialised at run time is guarded, and the
// guard waits for the thread running the initialiser.
// CHECK-DAG: error: wtc: guarded() is declared lock-free but can block
// CHECK-DAG: note: which takes a static-initialisation lock (__cxa_guard_acquire)
WTC_LOCK_FREE long guarded() { static long once = plain * 2; return once; }

// A throw is spelled as operations, not as a call into the runtime, and
// blocks all the same.
// CHECK-DAG: error: wtc: throws() is declared lock-free but can block
// CHECK-DAG: note: which throws: the C++ runtime allocates the exception and unwinds the stack
WTC_LOCK_FREE void throws() { if (plain < 0) throw plain; }

// A weak body may be replaced at link time: it is not followed.
// CHECK-DAG: error: wtc: weakly() is declared lock-free but can block
// CHECK-DAG: note: which calls weak_hook(), whose body here may be replaced at link time, so it is counted as blocking
__attribute__((weak)) void weak_hook() {}
WTC_LOCK_FREE void weakly() { weak_hook(); }

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

// Inline assembly is not looked into, except when it is made only of
// instructions that cannot wait: prefetches, the spin hint (also spelled
// "rep; nop"), fences, the time-stamp counter, the stack fence, or nothing
// at all. DPDK's fast path uses them.
// CHECK-DAG: remark: wtc: spin_hints() is declared lock-free: nothing it reaches blocks
WTC_LOCK_FREE void spin_hints() {
    asm volatile("prefetcht0 %0" : : "m"(plain));
    asm volatile("pause");
    asm volatile("pause # wait a little");
    asm volatile("rep; nop" : : : "memory");
    asm volatile("rep; nop # spin" : : : "memory");
    asm volatile("mfence\n\tlfence" : : : "memory");
    asm volatile("lock addl $0, -128(%%rsp); " : : : "memory");
    asm volatile("lock; addl $0,-4(%%rsp)" : : : "memory");
    asm volatile("" : : : "memory");
    asm volatile("# a comment only");
    long lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    plain += lo + hi;
}

// Anything else counts: another instruction after an allowed one, however
// the statements are separated (a carriage return ends one too), an
// allowed mnemonic with operands it does not take, a locked instruction on
// memory other than a slot below the stack pointer, or an operand where
// the fence's immediate zero belongs.
// CHECK-DAG: error: wtc: traps() is declared lock-free but can block
// CHECK-DAG: note: which runs inline assembly, which is not looked into
WTC_LOCK_FREE void traps() { asm volatile("pause; syscall" : : : "memory"); }
// CHECK-DAG: error: wtc: traps_after_cr() is declared lock-free but can block
WTC_LOCK_FREE void traps_after_cr() { asm volatile("pause \rsyscall" : : : "memory"); }
// A comment ends at the end of its statement: what follows a carriage
// return or a newline is another instruction, not more of the comment.
// CHECK-DAG: error: wtc: traps_after_comment_cr() is declared lock-free but can block
WTC_LOCK_FREE void traps_after_comment_cr() { asm volatile("pause # x\rsyscall" : : : "memory"); }
// CHECK-DAG: error: wtc: traps_after_comment_nl() is declared lock-free but can block
WTC_LOCK_FREE void traps_after_comment_nl() { asm volatile("pause # x\nsyscall" : : : "memory"); }
// CHECK-DAG: error: wtc: pause_with_operand() is declared lock-free but can block
WTC_LOCK_FREE void pause_with_operand() { asm volatile("pause %0" : : "r"(plain)); }
// CHECK-DAG: error: wtc: locked_shared() is declared lock-free but can block
WTC_LOCK_FREE void locked_shared() { asm volatile("lock addl $1, %0" : "+m"(plain)); }
// CHECK-DAG: error: wtc: locked_cmpxchg() is declared lock-free but can block
WTC_LOCK_FREE void locked_cmpxchg() {
    long old = 0;
    asm volatile("lock; cmpxchgq %2, %1" : "+a"(old), "+m"(plain) : "r"(1L) : "memory");
}
// CHECK-DAG: error: wtc: exchanges() is declared lock-free but can block
WTC_LOCK_FREE void exchanges() { long v = 1; asm volatile("xchg %0, %1" : "+r"(v), "+m"(plain)); }
// CHECK-DAG: error: wtc: fence_off_stack() is declared lock-free but can block
WTC_LOCK_FREE void fence_off_stack() { asm volatile("lock addl $0,-128(%%rax)" : : : "memory"); }
// CHECK-DAG: error: wtc: fence_above_stack() is declared lock-free but can block
WTC_LOCK_FREE void fence_above_stack() { asm volatile("lock addl $0, 8(%%rsp)" : : : "memory"); }
// CHECK-DAG: error: wtc: fence_by_or() is declared lock-free but can block
WTC_LOCK_FREE void fence_by_or() { asm volatile("lock orl $0, -128(%%rsp)" : : : "memory"); }
// CHECK-DAG: error: wtc: fence_of_operand() is declared lock-free but can block
WTC_LOCK_FREE void fence_of_operand() { asm volatile("lock addl %0, -128(%%rsp)" : : "i"(0) : "memory"); }
// CHECK-DAG: error: wtc: spin_until() is declared lock-free but can block
WTC_LOCK_FREE void spin_until() { asm volatile("1: pause; cmpq $0, %0; jnz 1b" : : "m"(plain)); }
// CHECK-DAG: error: wtc: waits_for_store() is declared lock-free but can block
WTC_LOCK_FREE void waits_for_store() { asm volatile("umwait %0" : : "r"(0) : "memory"); }

// Not declared: nothing is said about it, whatever it does.
// CHECK-NOT: undeclared
void undeclared() { std::printf("x\n"); }

int main() {
    locked(); logs(); allocates(); hooked(); unseen(); recursive(0); clean(); undeclared();
    throws(); known_target(); known_blocking_target(); waits(); sleeps();
    two_sites(); spin_hints(); traps(); traps_after_cr(); traps_after_comment_cr();
    traps_after_comment_nl(); pause_with_operand(); locked_shared();
    locked_cmpxchg(); exchanges(); fence_off_stack(); fence_above_stack(); fence_by_or();
    fence_of_operand(); spin_until(); waits_for_store();
}
