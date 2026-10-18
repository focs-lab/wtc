// One producer, one consumer, as fast as they can go: kItems ints through
// one queue. The consumer spins on an empty queue. Measures throughput
// under saturation; latency here would only measure how far behind the
// consumer falls, so none is reported.

#include "../bench.hpp"
#include "thread.hpp"

namespace {
constexpr long kItems = 5'000'000;
const char *const kName = "spsc_throughput";
}

bench_queue<int> g_queue;

long g_received = 0;
long g_sum = 0;

void producer() {
    for (long i = 0; i < kItems; ++i) {
        int value = static_cast<int>(i);
        g_queue.push(value);
    }
}

void consumer() {
    long expected = 0;
    while (g_received < kItems) {
        auto value = BENCH_POP(g_queue);
        if (value) {
            if (*value != expected)
                bench::fail(kName, "out of order or duplicated", *value);
            g_sum += *value;
            ++expected;
            ++g_received;
        }
    }
}

int main() {
    std::int64_t start = bench::now_ns();
    auto p = wtc::spawn(producer);
    auto c = wtc::spawn(consumer);
    p.join();
    c.join();
    std::int64_t elapsed = bench::now_ns() - start;

    if (g_sum != kItems * (kItems - 1) / 2)
        bench::fail(kName, "wrong sum", g_sum);
    bench::report(kName, kItems, elapsed);
    return 0;
}
