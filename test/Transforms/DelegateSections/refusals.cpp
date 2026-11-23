// Sections the pass must not delegate, one per reason, each with the
// condition it fails (docs/delegation.md, "The legality proof"):
//
//   (a) the section can tell which thread runs it;
//   (b) it is not a section of one entry and exits at the mutex's unlock;
//   (c) it takes a lock it does not let go, or a recursive one;
//   (d) it waits for another thread other than by such a lock;
//   (e) the pass cannot see all of its code.
//
// RUN: %clangxx -std=c++17 -O1 -fno-exceptions -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=S::mu_ -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections="mutex=S::mu_ assume-uniform-threads=true" -o /dev/null 2>&1 | FileCheck %s --check-prefix=UNIFORM

#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>

thread_local long per_thread;
extern long opaque(long) noexcept;
void work() {}

struct S {
    std::mutex mu_;
    std::mutex other_;
    std::recursive_mutex rec_;
    std::condition_variable cv_;
    long n = 0;
    bool ready = false;
    long (*hook)(long) = nullptr;

    long tls();
    long reads_errno();
    long thread_id();
    void *allocates();
    void starts();
    void leaks(bool b);
    void relock();
    void keeps_other();
    void recursive();
    void waits();
    void sleeps();
    void prints();
    long opaque_call();
    long through_pointer();
    long fenced();
    long cpu();
    void fine();
};

// (a) thread-local storage, errno, the thread's id, the C allocator, a
// thread started inside the section.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::tls #1 on S::mu_ not delegated: (a) depends on the thread that runs it: it reads the thread-local per_thread (through thread-local wrapper routine for per_thread)
long S::tls() { std::lock_guard<std::mutex> g(mu_); return n + per_thread; }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::reads_errno #1 on S::mu_ not delegated: (a) depends on the thread that runs it: it calls __errno_location
long S::reads_errno() { std::lock_guard<std::mutex> g(mu_); return n + errno; }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::thread_id #1 on S::mu_ not delegated: (a) depends on the thread that runs it: it calls pthread_self
long S::thread_id() { std::lock_guard<std::mutex> g(mu_); return n + (long)pthread_self(); }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::allocates #1 on S::mu_ not delegated: (a) depends on the thread that runs it: malloc reports a failure through errno, which is the server's
void *S::allocates() { std::lock_guard<std::mutex> g(mu_); return std::malloc(n); }
// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section S::starts #1 on S::mu_ not delegated: (a) starts a thread {{.*}}which inherits the signal mask, affinity and scheduling of the thread that runs it (assume-uniform-threads admits it)
// UNIFORM-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::starts #1 on S::mu_ delegated
void S::starts() { std::lock_guard<std::mutex> g(mu_); std::thread(work).detach(); }

// (b) a section that does not end in the function, and one that takes the
// mutex again before letting it go.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::leaks #1 on S::mu_ not delegated: (b) leaves S::leaks with the mutex held
void S::leaks(bool b) { mu_.lock(); if (b) return; ++n; mu_.unlock(); }
// CHECK-DAG: [[@LINE+2]]:{{.*}} remark: wtc: delegated section S::relock #1 on S::mu_ not delegated: (b) takes the mutex again before letting it go
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::relock #2 on S::mu_ not delegated: (b) is reached with the mutex held, inside an earlier section
void S::relock() { mu_.lock(); ++n; mu_.lock(); mu_.unlock(); }

// (c) another lock taken and kept; a recursive mutex.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::keeps_other #1 on S::mu_ not delegated: (c) takes a lock it does not let go within the section, or lets go one it did not take
void S::keeps_other() { std::lock_guard<std::mutex> g(mu_); other_.lock(); ++n; }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::recursive #1 on S::mu_ not delegated: (c) takes a recursive mutex (std::lock_guard<std::recursive_mutex>::lock_guard(std::recursive_mutex&)): the server is not the thread that may hold it
void S::recursive() { std::lock_guard<std::mutex> g(mu_); std::lock_guard<std::recursive_mutex> r(rec_); ++n; }

// (d) a condition wait, a sleep, output.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::waits #1 on S::mu_ not delegated: (d) waits in the threading library (std::condition_variable::wait(std::unique_lock<std::mutex>&))
void S::waits() { std::unique_lock<std::mutex> l(mu_); while (!ready) cv_.wait(l); ++n; }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::sleeps #1 on S::mu_ not delegated: (d) sleeps or yields (usleep)
void S::sleeps() { std::lock_guard<std::mutex> g(mu_); usleep(10); ++n; }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::prints #1 on S::mu_ not delegated: (d) does input or output (printf)
void S::prints() { std::lock_guard<std::mutex> g(mu_); std::printf("%ld\n", n); }

// (e) a function with no body here, a call through a pointer the pass
// cannot resolve, inline assembly that reads the CPU.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::opaque_call #1 on S::mu_ not delegated: (e) calls opaque(long), which has no body here and is not on the audited list
long S::opaque_call() { std::lock_guard<std::mutex> g(mu_); return opaque(n); }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::through_pointer #1 on S::mu_ not delegated: (e) makes an indirect call the pass cannot resolve
long S::through_pointer() { std::lock_guard<std::mutex> g(mu_); return hook(n); }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::cpu #1 on S::mu_ not delegated: (e) runs inline assembly the pass does not read
long S::cpu() { std::lock_guard<std::mutex> g(mu_); unsigned lo, hi, c; asm volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(c)); return n + c; }

// Inline assembly that only orders memory, and an atomic, are no obstacle.
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::fenced #1 on S::mu_ delegated
long S::fenced() { std::lock_guard<std::mutex> g(mu_); asm volatile("mfence" ::: "memory"); return ++n; }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section S::fine #1 on S::mu_ delegated
void S::fine() { std::lock_guard<std::mutex> g(mu_); std::lock_guard<std::mutex> o(other_); ++n; }

// The module is the whole program (it has main and nothing is linked), so
// the virtual call that destroys std::thread's state resolves to the
// module's vtables.
int main() { return 0; }

// A refused section is left as written.
// IR-LABEL: cir.func {{.*}}@_ZN1S5waitsEv(
// IR-NOT: wtc.delegate
// IR: cir.return
