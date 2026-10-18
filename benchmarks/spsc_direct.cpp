// The same workload as queue_bench.cpp, written by hand against the
// substitute with its allocation-free pop. This is the ceiling: the
// compiler cannot beat it, only reach it without the human.

#include "../runtime/spsc_queue.hpp"
#include "thread.hpp"

#include <chrono>
#include <cstdio>

constexpr long kNumItems = 5'000'000;

wtc::runtime::spsc_queue<int> g_queue;

long g_received = 0;
long g_sum = 0;
long g_out_of_order = 0;

void producer() {
    for (long i = 0; i < kNumItems; ++i) {
        int value = static_cast<int>(i);
        g_queue.push(value);
    }
}

void consumer() {
    long expected = 0;
    while (g_received < kNumItems) {
        auto value = g_queue.try_pop_optional();
        if (value) {
            if (*value != expected)
                ++g_out_of_order;
            g_sum += *value;
            ++expected;
            ++g_received;
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

    long expected_sum = kNumItems * (kNumItems - 1) / 2;
    if (g_received != kNumItems || g_sum != expected_sum || g_out_of_order != 0) {
        std::printf("FAIL received=%ld sum=%ld (expected %ld) out_of_order=%ld\n",
                    g_received, g_sum, expected_sum, g_out_of_order);
        return 1;
    }

    std::printf("items=%ld elapsed_ms=%.3f items_per_sec=%.0f\n",
                kNumItems, seconds * 1000.0, static_cast<double>(kNumItems) / seconds);
    return 0;
}
