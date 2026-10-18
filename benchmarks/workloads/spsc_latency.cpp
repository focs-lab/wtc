// One producer, one consumer, at a fixed rate well below either queue's
// capacity: one item every kIntervalNs. Each item is the time it was
// pushed, so the consumer can measure how long it waited in the queue.
// Reports the latency distribution, the number the project's motivation
// rests on: p50, p99, p99.9 and the worst case.
//
// The producer paces itself by spinning on the clock rather than sleeping,
// because a sleep's own wake-up jitter would swamp what is being measured.

#include "../bench.hpp"
#include "thread.hpp"

namespace {
constexpr long kItems = 1'000'000;
constexpr std::int64_t kIntervalNs = 1'000;
const char *const kName = "spsc_latency";
}

bench_queue<std::int64_t> g_queue;

std::vector<std::int64_t> g_latencies;
long g_received = 0;

void producer() {
    std::int64_t next = bench::now_ns();
    for (long i = 0; i < kItems; ++i) {
        while (bench::now_ns() < next) {
        }
        std::int64_t pushed_at = bench::now_ns();
        g_queue.push(pushed_at);
        next += kIntervalNs;
    }
}

void consumer() {
    std::int64_t previous = 0;
    while (g_received < kItems) {
        auto value = BENCH_POP(g_queue);
        if (value) {
            std::int64_t popped_at = bench::now_ns();
            if (*value < previous)
                bench::fail(kName, "out of order", g_received);
            previous = *value;
            g_latencies[g_received] = popped_at - *value;
            ++g_received;
        }
    }
}

int main() {
    g_latencies.resize(kItems);

    std::int64_t start = bench::now_ns();
    auto c = wtc::spawn(consumer);
    auto p = wtc::spawn(producer);
    p.join();
    c.join();
    std::int64_t elapsed = bench::now_ns() - start;

    bench::report(kName, kItems, elapsed, &g_latencies);
    return 0;
}
