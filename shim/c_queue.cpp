// The runtime behind shim/c_queue.h: a lifted C queue is a bounded ring of
// pointers (runtime/bounded_ring.hpp) with an eventcount for each side
// (runtime/waiter.hpp).
// tools/wtc-cc compiles this file with the program's WTC_* definitions and
// links it into every C program it builds.

#include "c_queue.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "topology.hpp"
#include "../runtime/bounded_ring.hpp"
#include "../runtime/waiter.hpp"

namespace {

using wtc::topology;

bool manyProducers(topology t) { return t != topology::spsc && t != topology::spsc_inline && t != topology::spmc; }
bool manyConsumers(topology t) { return t != topology::spsc && t != topology::spsc_inline && t != topology::mpsc; }

struct c_queue {
    c_queue(topology tag, std::size_t capacity)
        : ring(capacity, manyProducers(tag), manyConsumers(tag), /*overrun=*/false, /*batches=*/true),
          not_empty(wtc::rt::wait_policy::spin_then_park),
          not_full(wtc::rt::wait_policy::spin_then_park) {}

    wtc::rt::bounded_ring<void*> ring;
    // Their own lines: a waiter that goes to sleep or wakes writes them.
    alignas(128) wtc::rt::waiter not_empty;
    alignas(128) wtc::rt::waiter not_full;
};

c_queue* get(void* at) {
    c_queue* q;
    std::memcpy(&q, at, sizeof q);
    return q;
}

[[noreturn]] void fail(const char* what, void* at) {
    std::fprintf(stderr, "wtc: lifted C queue %p: %s\n", at, what);
    std::abort();
}

// The caller's own ring, as the program keeps it.
struct own_ring {
    void** data;
    int* in;
    int* out;
    unsigned long slots;

    unsigned long count() const {
        return (static_cast<unsigned long>(*in) + slots - static_cast<unsigned long>(*out)) % slots;
    }
    unsigned long room() const { return slots - 1 - count(); }
    void insert(void* v) {
        data[*in] = v;
        *in = static_cast<int>((static_cast<unsigned long>(*in) + 1) % slots);
    }
    void* remove() {
        void* v = data[*out];
        *out = static_cast<int>((static_cast<unsigned long>(*out) + 1) % slots);
        return v;
    }
};

unsigned long least(unsigned long a, int limit) {
    if (limit <= 0) return 0;
    return a < static_cast<unsigned long>(limit) ? a : static_cast<unsigned long>(limit);
}

}  // namespace

extern "C" {

void wtc_c_queue_init(void* at, int tag, long capacity) {
    if (capacity < 0) fail("built with a negative capacity", at);
    c_queue* q = new c_queue(static_cast<topology>(tag), static_cast<std::size_t>(capacity));
    std::memcpy(at, &q, sizeof q);
}

void wtc_c_queue_destroy(void* at) { delete get(at); }

void wtc_c_queue_push_wait(void* at, void* v) {
    c_queue* q = get(at);
    q->not_full.wait([&]() noexcept { return q->ring.try_push(v); });
    q->not_empty.notify(1);
}

int wtc_c_queue_pop_wait(void* at, void** out) {
    c_queue* q = get(at);
    q->not_empty.wait([&]() noexcept { return q->ring.consume([&](void*&& e) noexcept { *out = e; }); });
    q->not_full.notify(1);
    return 1;
}

int wtc_c_queue_push_batch(void* at, void*** data, int* in, int* out, unsigned long slots, int limit) {
    c_queue* q = get(at);
    own_ring src{*data, in, out, slots};
    const unsigned long n = least(src.count(), limit);
    if (n == 0) {
        q->not_full.wait([&]() noexcept { return q->ring.size() < q->ring.capacity(); });
        return 0;
    }
    std::size_t k = 0;
    q->not_full.wait([&]() noexcept {
        k = q->ring.try_push_n(n, [&](std::size_t) noexcept { return src.remove(); });
        return k > 0;
    });
    q->not_empty.notify(k);
    return static_cast<int>(k);
}

int wtc_c_queue_pop_batch(void* at, void*** data, int* in, int* out, unsigned long slots, int limit) {
    c_queue* q = get(at);
    own_ring dst{*data, in, out, slots};
    const unsigned long n = least(dst.room(), limit);
    if (n == 0) {
        q->not_empty.wait([&]() noexcept { return q->ring.size() > 0; });
        return 0;
    }
    std::size_t k = 0;
    q->not_empty.wait([&]() noexcept {
        k = q->ring.consume_n(n, [&](void*&& e) noexcept { dst.insert(e); });
        return k > 0;
    });
    q->not_full.notify(k);
    return static_cast<int>(k);
}

}  // extern "C"
