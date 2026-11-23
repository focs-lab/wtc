// Built with exceptions: a section through which one may pass would unwind
// the server instead of the client, and is refused, whether the call that
// may throw is the lock, its unlock (a std::mutex lets go in a call the
// front end guards with a cleanup) or a call in between. A section whose
// calls cannot throw (the C library's, which are nothrow) is delegated.
//
// RUN: %clangxx -std=c++17 -O1 -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=T::mu_ -o /dev/null 2>&1 | FileCheck %s
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=gm -o /dev/null 2>&1 | FileCheck %s --check-prefix=C

#include <mutex>
#include <pthread.h>

long may_throw(long x) {
    if (x < 0) throw x;
    return x;
}

pthread_mutex_t gm = PTHREAD_MUTEX_INITIALIZER;
long total;

struct T {
    std::mutex mu_;
    long n = 0;
    long risky(long x);
    long unlocks(long x);
};

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section T::risky on T::mu_ not delegated: (b) an exception may leave the section through its lock: it would unwind the server, not the client
long T::risky(long x) { std::lock_guard<std::mutex> g(mu_); n += may_throw(x); return n; }
// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section T::unlocks #1 on T::mu_ not delegated: (b) an exception may leave the section through a call of std::mutex::unlock()
long T::unlocks(long x) { mu_.lock(); n += x; long r = n; mu_.unlock(); return r; }

// C-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section c_add #1 on gm delegated
long c_add(long x) { pthread_mutex_lock(&gm); total += x; long r = total; pthread_mutex_unlock(&gm); return r; }
// C-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section c_risky #1 on gm not delegated: (b) an exception may leave the section through a call of may_throw: it would unwind the server, not the client
long c_risky(long x) { pthread_mutex_lock(&gm); total += may_throw(x); pthread_mutex_unlock(&gm); return total; }

// The whole program: no file the pass does not see may make gm recursive.
int main() { return 0; }
