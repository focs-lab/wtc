// Sections the pass delegates, in every form it reads: a std::lock_guard,
// a std::unique_lock let go early, a lock object of the program's whose
// constructor and destructor call lock and unlock functions of a class
// that wraps std::mutex (LevelDB's MutexLock and port::Mutex), a section
// that unlocks inside a nested block and one that locks again before a
// return the lock object's destructor ends, and sections that hand results
// back through a local, a member and the return value.
//
// RUN: %clangxx -std=c++17 -O1 -fno-exceptions -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=DB::mu_,GetDB::mu_ -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir
// RUN: wtc-opt %t.out.cir --lower-wtc-to-cir --reconcile-unrealized-casts -o %t.low.cir
// RUN: FileCheck %s --check-prefix=LOW < %t.low.cir
// RUN: cir-translate --cir-to-llvmir %t.low.cir -o /dev/null
// RUN: wtc-opt %t.cir --wtc-delegate-sections="mutex=DB::mu_,GetDB::mu_ rewrite=false" -o %t.scan.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=SCAN < %t.scan.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=DB::nothing -o /dev/null 2>&1 | FileCheck %s --check-prefix=NONE

#include <mutex>

namespace port {
class Mutex {
public:
    void Lock() { mu_.lock(); }
    void Unlock() { mu_.unlock(); }

private:
    std::mutex mu_;
};
}  // namespace port

class MutexLock {
public:
    explicit MutexLock(port::Mutex *mu) : mu_(mu) { mu_->Lock(); }
    ~MutexLock() { mu_->Unlock(); }

private:
    port::Mutex *const mu_;
};

struct Version {
    int refs = 0;
    long seq = 0;
    void Ref() { ++refs; }
    void Unref() { --refs; }
};

long lookup(long key, long seq);

class DB {
public:
    long get(long key);
    long snapshot();
    void add(long x);
    long counted(long x);
    long early(long x);

private:
    std::mutex mu_;
    port::Mutex unused_;
    Version *current_ = nullptr;
    long last_ = 0;
    long count_ = 0;
};

// LevelDB's DBImpl::Get, reduced: two sections around a lookup made
// unlocked, the second ending at the lock object's destructor on return.
struct GetDB {
    port::Mutex mu_;
    Version *current_ = nullptr;
    long last_ = 0;
    long get(long key);
};

// CHECK-DAG: [[@LINE+3]]:{{.*}} remark: wtc: delegated section GetDB::get #1 on GetDB::mu_ delegated
// CHECK-DAG: [[@LINE+10]]:{{.*}} remark: wtc: delegated section GetDB::get #2 on GetDB::mu_ delegated
long GetDB::get(long key) {
    MutexLock l(&mu_);
    long seq = last_;
    Version *v = current_;
    v->Ref();
    long r;
    {
        mu_.Unlock();
        r = lookup(key, seq);
        mu_.Lock();
    }
    v->Unref();
    return r + v->seq;
}

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section DB::snapshot #1 on DB::mu_ delegated
long DB::snapshot() { std::lock_guard<std::mutex> g(mu_); return ++last_; }

// CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section DB::add #1 on DB::mu_ delegated
void DB::add(long x) { std::lock_guard<std::mutex> g(mu_); count_ += x; }

// A result handed back through a local written in the section.
long DB::counted(long x) {
    long before;
    {
        // CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section DB::counted #1 on DB::mu_ delegated
        std::lock_guard<std::mutex> g(mu_);
        before = count_;
        count_ += x;
    }
    return before * 2;
}

// A std::unique_lock let go before the end of its scope: the section ends
// at its unlock, and the code after it stays on the caller.
long DB::early(long x) {
    // CHECK-DAG: [[@LINE+1]]:{{.*}} remark: wtc: delegated section DB::early #1 on DB::mu_ delegated
    std::unique_lock<std::mutex> l(mu_);
    long seen = count_;
    l.unlock();
    return lookup(x, seen);
}

// IR: wtc.delegate %{{.*}}, @[[S:_ZN5GetDB3getEl.wtc.section.[0-9]+]](%{{.*}}) {mutex_name = "GetDB::mu_"} : !cir.ptr<!rec_port3A3AMutex>, !cir.ptr<!void> -> !s32i
// IR: cir.func internal private @_ZN5GetDB3getEl.wtc.section.{{[0-9]+}}(%arg0: !cir.ptr<!void>) -> !s32i {{.*}}wtc.delegated_section = "GetDB::mu_"
// LOW-NOT: wtc.delegate
// LOW: cir.call @wtc_delegate_run(
// LOW: cir.func private @wtc_delegate_run(!cir.ptr<!void>, !cir.ptr<!cir.func<(!cir.ptr<!void>) -> !s32i>>, !cir.ptr<!void>) -> !s32i
// SCAN-NOT: wtc.delegate
// SCAN-NOT: wtc.section
// NONE: remark: wtc: delegated section mutex DB::nothing has no section here
