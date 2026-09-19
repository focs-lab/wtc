#include "queue.hpp"
#include "spsc_queue.hpp"
#include "thread.hpp"

#include <chrono>
#include <cstdio>

constexpr long kNumItems = 5'000'000;

wtc::queue<int> g_queue;

// Anchor: makes wtc::spsc_queue<int,8388608>'s real implementation
// available in this translation unit for --select-queue-impl to find and
// substitute in place of g_queue's declared type. Never called directly.
//
// Capacity is set above kNumItems deliberately: the substituted push() can
// return false once the ring buffer is full, and today's lowering discards
// that result exactly like the original's void push -- there is no retry,
// so an overflow means silently dropped items rather than backpressure.
// Sizing capacity above the total item count makes overflow mathematically
// impossible for this workload regardless of any producer/consumer rate
// imbalance, sidestepping the gap rather than fixing it. The real fix
// belongs in LowerQueuePushPattern (synthesize a retry loop when the
// target's push can fail) -- real, separate work, not done here.
template class wtc::spsc_queue<int, 1 << 23>;

void producer() {
    for (long i = 0; i < kNumItems; ++i) {
        int value = static_cast<int>(i);
        g_queue.push(value);
    }
}

void consumer() {
    long received = 0;
    while (received < kNumItems) {
        auto value = g_queue.try_pop();
        if (value) {
            ++received;
        }
    }
}

int main() {
    auto start = std::chrono::steady_clock::now();

    auto producer_thread = wtc::spawn(producer);
    auto consumer_thread = wtc::spawn(consumer);
    producer_thread.join();
    consumer_thread.join();

    auto end = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();
    double items_per_sec = static_cast<double>(kNumItems) / seconds;

    std::printf("items=%ld elapsed_ms=%.3f items_per_sec=%.0f\n",
                kNumItems, seconds * 1000.0, items_per_sec);

    return 0;
}
