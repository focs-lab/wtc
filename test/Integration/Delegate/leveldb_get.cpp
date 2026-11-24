// A reduced model of LevelDB's DB mutex (db/db_impl.cc), through the whole
// pipeline with its sections delegated (docs/delegation.md). As in LevelDB:
//
//   - port::Mutex wraps std::mutex, MutexLock is the lock object, and
//     DB::Get holds the mutex in two sections around a lookup made
//     unlocked: the first takes a snapshot and references the memtable and
//     the version, the second updates the seek statistics, may schedule a
//     compaction through a virtual Env::Schedule, whose first call starts
//     the background thread, and drops the references, a reference that
//     reaches zero deleting the object;
//   - a write waits on a condition variable of the same mutex while a
//     compaction is scheduled, so its section is refused and runs inline
//     under the mutex, which the server takes too;
//   - the background thread's section, which signals the writers, is
//     delegated: the background thread is a client like the readers.
//
// Six readers check every value they find against the one its key was
// written with, a sixth of their keys never written, so that misses run
// down the version's seeks and schedule compactions; a writer adds keys;
// the objects' count must come back to the memtable and version alive at
// the end. Run as written, delegated (every section, and by the adaptive
// runtime's choice), and delegated under ThreadSanitizer the same two
// ways.
//
// RUN: %wtc_cc -O1 -fno-exceptions %s -o %t.plain.exe 2>&1 | FileCheck %s --allow-empty --check-prefix=PLAIN
// RUN: %t.plain.exe | FileCheck %s
// RUN: %wtc_cc -O1 -fno-exceptions --delegate DB::mutex_ --delegate-assume-uniform-threads %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=BUILD
// RUN: env WTC_DELEGATE_REPORT=1 WTC_DELEGATE_MODE=always %t.exe 2>&1 | FileCheck %s --check-prefixes=CHECK,SERVED
// RUN: env WTC_DELEGATE_REPORT=1 %t.exe 2>&1 | FileCheck %s --check-prefixes=CHECK,ADAPT
// RUN: %wtc_cc -O1 -fno-exceptions --delegate DB::mutex_ %s -o %t.strict.exe 2>&1 | FileCheck %s --check-prefix=STRICT
// RUN: %t.strict.exe | FileCheck %s
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g -fno-exceptions --delegate DB::mutex_ --delegate-assume-uniform-threads %s -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefix=BUILD %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 WTC_DELEGATE_REPORT=1 WTC_DELEGATE_MODE=always %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,CHECK,SERVED %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 WTC_DELEGATE_REPORT=1 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,CHECK,ADAPT %}
//
// PLAIN-NOT: delegated section
// BUILD-DAG: remark: wtc: delegated section DB::Get #1 on DB::mutex_ delegated
// BUILD-DAG: remark: wtc: delegated section DB::Get #2 on DB::mutex_ delegated
// BUILD-DAG: remark: wtc: delegated section DB::BackgroundCall #1 on DB::mutex_ delegated
// BUILD-DAG: remark: wtc: delegated section DB::Put #1 on DB::mutex_ not delegated: (d) waits in the threading library
// STRICT-DAG: remark: wtc: delegated section DB::Get #1 on DB::mutex_ delegated
// STRICT-DAG: remark: wtc: delegated section DB::Get #2 on DB::mutex_ not delegated: (a) starts a thread
// TSAN-NOT: ThreadSanitizer
// CHECK: errors 0 live 2
// SERVED: wtc-delegate: server 0 ran {{[1-9][0-9]*}} sections
// ADAPT: wtc-delegate: clients: {{[1-9][0-9]*}} sections, free={{[0-9]+}} contended={{[0-9]+}} delegated={{[0-9]+}}

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace port {
class CondVar;
class Mutex {
public:
    void Lock() { mu_.lock(); }
    void Unlock() { mu_.unlock(); }

private:
    friend class CondVar;
    std::mutex mu_;
};

class CondVar {
public:
    explicit CondVar(Mutex *mu) : mu_(mu) {}
    void Wait() {
        std::unique_lock<std::mutex> lock(mu_->mu_, std::adopt_lock);
        cv_.wait(lock);
        lock.release();
    }
    void SignalAll() { cv_.notify_all(); }

private:
    std::condition_variable cv_;
    Mutex *const mu_;
};
}  // namespace port

class MutexLock {
public:
    explicit MutexLock(port::Mutex *mu) : mu_(mu) { mu_->Lock(); }
    ~MutexLock() { mu_->Unlock(); }
    MutexLock(const MutexLock &) = delete;
    MutexLock &operator=(const MutexLock &) = delete;

private:
    port::Mutex *const mu_;
};

class Env {
public:
    virtual ~Env() = default;
    virtual void Schedule(void (*fn)(void *), void *arg) = 0;
};

// LevelDB's PosixEnv: one background thread, started by the first
// Schedule, which runs the work queued for it.
class PosixEnv : public Env {
public:
    void Schedule(void (*fn)(void *), void *arg) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!started_) {
            started_ = true;
            std::thread(&PosixEnv::Run, this).detach();
        }
        if (queue_.empty()) cv_.notify_one();
        queue_.emplace_back(fn, arg);
    }

private:
    static void Run(PosixEnv *env) {
        for (;;) {
            std::unique_lock<std::mutex> lock(env->mu_);
            while (env->queue_.empty()) env->cv_.wait(lock);
            auto work = env->queue_.front();
            env->queue_.pop_front();
            lock.unlock();
            work.first(work.second);
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    bool started_ = false;
    std::deque<std::pair<void (*)(void *), void *>> queue_;
};

long live;  // objects alive, counted under the DB mutex

// A memtable: keys and values, never changed once built (LevelDB's skip
// list is read without the mutex).
struct MemTable {
    std::vector<std::pair<long, long>> rows;
    int refs = 0;
    MemTable() { ++live; }
    ~MemTable() { --live; }
    void Ref() { ++refs; }
    void Unref() {
        if (--refs == 0) delete this;
    }
    bool Get(long key, long *value) const {
        size_t lo = 0, hi = rows.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (rows[mid].first < key) lo = mid + 1;
            else hi = mid;
        }
        if (lo < rows.size() && rows[lo].first == key) {
            *value = rows[lo].second;
            return true;
        }
        return false;
    }
};

struct Version {
    int refs = 0;
    long allowed_seeks = 16;
    Version() { ++live; }
    ~Version() { --live; }
    void Ref() { ++refs; }
    void Unref() {
        if (--refs == 0) delete this;
    }
    bool UpdateStats() { return --allowed_seeks == 0; }
};

class DB {
public:
    explicit DB(Env *env) : bg_cv_(&mutex_), env_(env) {
        mem_ = new MemTable;
        mem_->Ref();
        current_ = new Version;
        current_->Ref();
    }
    ~DB() {
        mem_->Unref();
        current_->Unref();
    }
    bool Get(long key, long *value);
    void Put(long key, long value);
    static void BGWork(void *db) { static_cast<DB *>(db)->BackgroundCall(); }

private:
    void MaybeScheduleCompaction() {
        if (background_compaction_scheduled_) return;
        background_compaction_scheduled_ = true;
        env_->Schedule(&DB::BGWork, this);
    }
    void BackgroundCall();

    port::Mutex mutex_;
    port::CondVar bg_cv_;
    Env *const env_;
    MemTable *mem_;
    Version *current_;
    long last_sequence_ = 0;
    bool background_compaction_scheduled_ = false;
};

bool DB::Get(long key, long *value) {
    MutexLock l(&mutex_);
    long snapshot = last_sequence_;
    MemTable *mem = mem_;
    Version *current = current_;
    mem->Ref();
    current->Ref();
    bool have_stat_update = false;
    bool found;
    {
        mutex_.Unlock();
        found = mem->Get(key, value) && snapshot >= 0;
        if (!found) have_stat_update = true;
        mutex_.Lock();
    }
    if (have_stat_update && current->UpdateStats()) MaybeScheduleCompaction();
    mem->Unref();
    current->Unref();
    return found;
}

void DB::Put(long key, long value) {
    MutexLock l(&mutex_);
    while (background_compaction_scheduled_) bg_cv_.Wait();
    MemTable *next = new MemTable;
    next->rows = mem_->rows;
    size_t at = 0;
    while (at < next->rows.size() && next->rows[at].first < key) ++at;
    next->rows.insert(next->rows.begin() + static_cast<long>(at), {key, value});
    next->Ref();
    mem_->Unref();
    mem_ = next;
    ++last_sequence_;
}

// A compaction: a new version in place of the current one.
void DB::BackgroundCall() {
    MutexLock l(&mutex_);
    Version *next = new Version;
    next->Ref();
    current_->Unref();
    current_ = next;
    background_compaction_scheduled_ = false;
    bg_cv_.SignalAll();
}

int main() {
    // Never destroyed, as LevelDB's default Env: its thread waits on.
    PosixEnv *env = new PosixEnv;
    DB *db = new DB(env);
    const long n = 400;
    for (long k = 0; k < n; ++k) db->Put(k, k * 3 + 1);
    std::vector<long> errors(6, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 6; ++t)
        threads.emplace_back([db, t, &errors] {
            unsigned x = 12345u + t;
            for (int i = 0; i < 20000; ++i) {
                x = x * 1103515245u + 12345u;
                long key = static_cast<long>((x >> 8) % (n + n / 2));
                long value = -1;
                bool found = db->Get(key, &value);
                if ((key < n && !found) || (found && value != key * 3 + 1)) ++errors[t];
            }
        });
    threads.emplace_back([db] {
        for (long k = n; k < n + n / 4; ++k) db->Put(k, k * 3 + 1);
    });
    for (auto &th : threads) th.join();
    long bad = 0;
    for (long e : errors) bad += e;
    // A write waits for a compaction still scheduled; after it nothing
    // else runs on the DB.
    db->Put(-1, -2);
    long alive = live;
    std::printf("errors %ld live %ld\n", bad, alive);
    std::fflush(stdout);  // before the runtime's report at exit
    return 0;
}
