// A producer that batches its pushes by hand, through the shim's batch
// API, and a consumer that checks every element arrives once and in order.
// TAG picks the implementation: 0 general-purpose, 1 spsc, 2 spsc_inline.

#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>
#include <cstdio>

#ifndef TAG
#define TAG 0
#endif

struct item {
    std::uint64_t seq;
};

wtc::queue<item> q(static_cast<wtc::topology>(TAG));
constexpr std::uint64_t kCount = 100003;  // not a multiple of the batch size
bool in_order = true;

void produce() {
    wtc::queue<item>::batch b;
    q.batch_begin(b);
    for (std::uint64_t i = 0; i < kCount; ++i) q.push_deferred(b, item{i});
    q.batch_end(b);
}

void consume() {
    for (std::uint64_t got = 0; got < kCount;) {
        auto p = q.try_pop();
        if (!p) continue;
        if (p->seq != got) in_order = false;
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
