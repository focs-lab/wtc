// A producer that pushes in a loop, marked WTC_BATCH, so the compiler may
// batch its pushes; the consumer checks every element arrives once and in
// order. TAG picks the implementation the program declares (0 leaves the
// choice to the compiler).

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>
#include <cstdio>

#ifndef TAG
#define TAG 0
#endif

struct packet {
    std::uint64_t seq;
    std::uint64_t payload[3];
};

wtc::queue<packet> q(static_cast<wtc::topology>(TAG));
constexpr std::uint64_t kCount = 100003;
bool in_order = true;

std::uint64_t checksum(std::uint64_t x) { return x * 0x9E3779B97F4A7C15ull; }

WTC_BATCH void produce() {
    for (std::uint64_t i = 0; i < kCount; ++i) {
        packet p{i, {checksum(i), 0, 0}};
        q.push(p);
    }
}

void consume() {
    for (std::uint64_t got = 0; got < kCount;) {
        auto p = q.try_pop();
        if (!p) continue;
        if (p->seq != got || p->payload[0] != checksum(got)) in_order = false;
        ++got;
    }
}

int main() {
    std::thread a = wtc::spawn(produce);
    std::thread b = wtc::spawn(consume);
    a.join();
    b.join();
    std::printf("tag %d in_order %d\n", static_cast<int>(q.tag()), in_order ? 1 : 0);
    return in_order ? 0 : 1;
}
