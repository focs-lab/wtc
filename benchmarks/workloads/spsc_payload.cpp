// spsc_throughput with a 64-byte record instead of an int, so each push
// and pop copies a cache line. Shows whether the substitution's advantage
// holds when the payload, not the synchronisation, starts to dominate, and
// that it applies to a user-defined element type at all.

#include "../bench.hpp"
#include "thread.hpp"

namespace {
constexpr long kItems = 2'000'000;
const char *const kName = "spsc_payload";
}

struct Payload {
    long seq;
    long words[7];
};

bench_queue<Payload> g_queue;

long g_received = 0;
long g_checksum = 0;

void producer() {
    for (long i = 0; i < kItems; ++i) {
        Payload payload;
        payload.seq = i;
        for (long w = 0; w < 7; ++w)
            payload.words[w] = i + w;
        g_queue.push(payload);
    }
}

void consumer() {
    while (g_received < kItems) {
        auto value = BENCH_POP(g_queue);
        if (value) {
            if (value->seq != g_received)
                bench::fail(kName, "out of order or duplicated", value->seq);
            if (value->words[6] != value->seq + 6)
                bench::fail(kName, "payload corrupted", value->seq);
            g_checksum += value->words[3];
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

    long expected = kItems * (kItems - 1) / 2 + 3 * kItems;
    if (g_checksum != expected)
        bench::fail(kName, "wrong checksum", g_checksum);
    bench::report(kName, kItems, elapsed);
    return 0;
}
