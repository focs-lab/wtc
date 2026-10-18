// One thread is both the producer and the consumer: it pushes a batch,
// then pops it all, kRounds times. The verdict is still SPSC (one thread
// pushes, one thread pops; they happen to be the same), and this is the
// case a bounded ring cannot take: with a batch larger than the ring, a
// push that waited for room would wait forever. The substitute is
// unbounded, so it must complete. With no second thread there is no
// contention at all, so this measures the queues' raw cost per operation.

#include "../bench.hpp"

namespace {
constexpr long kBatch = 200'000;
constexpr long kRounds = 10;
const char *const kName = "spsc_same_thread";
}

bench_queue<long> g_queue;

int main() {
    std::int64_t start = bench::now_ns();
    for (long round = 0; round < kRounds; ++round) {
        for (long i = 0; i < kBatch; ++i) {
            long value = round * kBatch + i;
            g_queue.push(value);
        }
        for (long i = 0; i < kBatch; ++i) {
            auto value = BENCH_POP(g_queue);
            if (!value)
                bench::fail(kName, "empty before the batch was drained", i);
            if (*value != round * kBatch + i)
                bench::fail(kName, "out of order or duplicated", *value);
        }
        if (BENCH_POP(g_queue))
            bench::fail(kName, "item left after the batch", round);
    }
    std::int64_t elapsed = bench::now_ns() - start;

    // Each item is pushed once and popped once; report operations as pairs.
    bench::report(kName, kBatch * kRounds, elapsed);
    return 0;
}
