// The control: two producers, one consumer. The verdict is MPSC, so
// --select-queue-impl must leave the queue alone, and run.sh fails if it
// does not. Baseline and optimized should then be the same program and
// measure the same, which bounds how much of the other workloads'
// difference is noise. There is no direct variant: spsc_queue is not safe
// with two producers.

#include "../bench.hpp"
#include "thread.hpp"

namespace {
constexpr long kItemsPerProducer = 2'500'000;
const char *const kName = "mpsc_control";
}

bench_queue<long> g_queue;

long g_received = 0;
long g_sum = 0;

void producer_even() {
    for (long i = 0; i < kItemsPerProducer; ++i) {
        long value = 2 * i;
        g_queue.push(value);
    }
}

void producer_odd() {
    for (long i = 0; i < kItemsPerProducer; ++i) {
        long value = 2 * i + 1;
        g_queue.push(value);
    }
}

void consumer() {
    // Each producer's own items must arrive in its order.
    long next_even = 0;
    long next_odd = 1;
    while (g_received < 2 * kItemsPerProducer) {
        auto value = BENCH_POP(g_queue);
        if (value) {
            long &next = (*value % 2 == 0) ? next_even : next_odd;
            if (*value != next)
                bench::fail(kName, "a producer's items out of order", *value);
            next += 2;
            g_sum += *value;
            ++g_received;
        }
    }
}

int main() {
    std::int64_t start = bench::now_ns();
    auto a = wtc::spawn(producer_even);
    auto b = wtc::spawn(producer_odd);
    auto c = wtc::spawn(consumer);
    a.join();
    b.join();
    c.join();
    std::int64_t elapsed = bench::now_ns() - start;

    long n = 2 * kItemsPerProducer;
    if (g_sum != n * (n - 1) / 2)
        bench::fail(kName, "wrong sum", g_sum);
    bench::report(kName, n, elapsed);
    return 0;
}
