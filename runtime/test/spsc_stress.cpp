// Stress test for runtime/spsc_queue.hpp. Build with -fsanitize=thread.
//
// One producer and one consumer move a long sequence through the queue.
// The consumer checks that every item arrives exactly once and in order. A
// small block size makes block hand-over, the delicate part, happen on
// almost every push. A second run leaves items in the queue at destruction
// and checks that every one of them is destroyed.

#include "spsc_queue.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

constexpr long kItems = 10'000'000;

[[noreturn]] void fail(const char *what, long detail) {
    std::fprintf(stderr, "FAIL: %s (%ld)\n", what, detail);
    std::exit(1);
}

template <std::size_t BlockSize>
void order_and_count() {
    wtc::runtime::spsc_queue<long, BlockSize> queue;

    std::thread producer([&] {
        for (long i = 0; i < kItems; ++i)
            queue.push(i);
    });

    long expected = 0;
    long sum = 0;
    while (expected < kItems) {
        if (auto value = queue.try_pop_optional()) {
            if (*value != expected)
                fail("out of order or duplicated", *value);
            sum += *value;
            ++expected;
        }
    }
    producer.join();

    if (queue.try_pop_optional())
        fail("item left after the last one", 0);
    if (sum != kItems * (kItems - 1) / 2)
        fail("wrong sum", sum);
}

std::atomic<long> live{0};

struct counted {
    long value;
    explicit counted(long v) : value(v) { live.fetch_add(1, std::memory_order_relaxed); }
    counted(const counted &other) : value(other.value) { live.fetch_add(1, std::memory_order_relaxed); }
    counted(counted &&other) noexcept : value(other.value) { live.fetch_add(1, std::memory_order_relaxed); }
    ~counted() { live.fetch_sub(1, std::memory_order_relaxed); }
};

void destroys_what_is_left() {
    {
        wtc::runtime::spsc_queue<counted, 4> queue;
        for (long i = 0; i < 103; ++i)
            queue.push(counted(i));
        for (long i = 0; i < 37; ++i) {
            auto popped = queue.try_pop();
            if (!popped || popped->value != i)
                fail("shared_ptr pop returned the wrong item", i);
        }
    }
    if (live.load() != 0)
        fail("items leaked or destroyed twice", live.load());
}

}

int main() {
    order_and_count<1>();
    order_and_count<4>();
    order_and_count<1024>();
    destroys_what_is_left();
    std::puts("spsc_stress: ok");
    return 0;
}
