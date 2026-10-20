#pragma once

#include <mutex>

// How many critical sections a batch holds the mutex across before letting
// it go once (see mutex::batch).
#ifndef WTC_LOCK_BATCH_K
#define WTC_LOCK_BATCH_K 64
#endif

namespace wtc {

class mutex {
private:
    std::mutex impl;

public:
    mutex() = default;
    ~mutex() = default;

    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    
    [[clang::annotate("wtc_mutex_lock")]]
    void lock() {
        impl.lock();
    }

    [[clang::annotate("wtc_mutex_unlock")]]
    void unlock() {
        impl.unlock();
    }

    // A run of critical sections held as one (--wtc-batch-locks):
    // batch_begin takes the mutex; batch_step ends one section, and every
    // WTC_LOCK_BATCH_K sections lets the mutex go and takes it again, so
    // that another thread can have it; batch_end lets it go. Which thread
    // gets it when it is let go is the mutex's choice, as it is when a
    // program unlocks and locks again. `batch` is the holder's count, in
    // storage the compiler allocates.
    struct batch {
        unsigned sections;
    };

    [[gnu::used, clang::annotate("wtc_mutex_batch_begin")]]
    void batch_begin(batch& b) {
        impl.lock();
        b.sections = 0;
    }

    [[gnu::used, clang::annotate("wtc_mutex_batch_step")]]
    void batch_step(batch& b) {
        if (++b.sections < WTC_LOCK_BATCH_K) return;
        b.sections = 0;
        impl.unlock();
        impl.lock();
    }

    [[gnu::used, clang::annotate("wtc_mutex_batch_end")]]
    void batch_end(batch&) {
        impl.unlock();
    }
};
    
}
