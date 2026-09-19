#include "spsc_queue.hpp"
#include "thread.hpp"

#include <chrono>
#include <cstdio>

constexpr long kNumItems = 5'000'000;

wtc::spsc_queue<int, 1 << 20> g_queue;

void producer() {
    for (long i = 0; i < kNumItems; ++i) {
        int value = static_cast<int>(i);
        g_queue.push(value);
    }
}

void consumer() {
    long received = 0;
    while (received < kNumItems) {
        auto value = g_queue.try_pop_optional();
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
