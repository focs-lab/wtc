// Two threads add to one counter under one mutex, each in a loop that takes
// and lets go of the mutex on every iteration. In a function marked
// WTC_BATCH the compiler holds the mutex across the loop instead and lets
// it go every WTC_LOCK_BATCH_K sections; the sum is the same either way.
// -DPLAIN leaves the mark off.

#include "mutex.hpp"
#include "progress.hpp"
#include "thread.hpp"

#include <cstdint>
#include <cstdio>

#ifdef PLAIN
#define ADDER
#else
#define ADDER WTC_BATCH
#endif

wtc::mutex m;
std::uint64_t total = 0;
constexpr std::uint64_t kCount = 200003;

ADDER void add() {
    for (std::uint64_t i = 0; i < kCount; ++i) {
        m.lock();
        total += i;
        m.unlock();
    }
}

int main() {
    std::thread a = wtc::spawn(add);
    std::thread b = wtc::spawn(add);
    a.join();
    b.join();
    const std::uint64_t want = kCount * (kCount - 1);
    std::printf("total %llu ok %d\n", static_cast<unsigned long long>(total), total == want ? 1 : 0);
    return total == want ? 0 : 1;
}
