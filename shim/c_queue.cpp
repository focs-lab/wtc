// The runtime behind shim/c_queue.h: a lifted C queue is a bounded ring of
// pointers (runtime/bounded_ring.hpp) with an eventcount for each side
// (runtime/waiter.hpp) and the count of producers that have finished.
// tools/wtc-cc compiles this file with the program's WTC_* definitions and
// links it into every C program it builds.

#include "c_queue.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>

#include "topology.hpp"
#include "../runtime/bounded_ring.hpp"
#include "../runtime/waiter.hpp"

namespace {

using wtc::topology;

bool manyProducers(topology t) { return t != topology::spsc && t != topology::spsc_inline && t != topology::spmc; }
bool manyConsumers(topology t) { return t != topology::spsc && t != topology::spsc_inline && t != topology::mpsc; }

// How a queue finishes (c_queue.h): never; when exactly `producers` have
// terminated, a termination more ending the program; when at least that
// many have, later ones changing nothing.
enum ends_mode { ends_never = 0, ends_exactly = 1, ends_at_least = 2 };

#ifdef WTC_C_QUEUE_TEST_HOOK
// runtime/test/c_queue_test.cpp: called where an operation has made its
// effect visible and still touches the queue.
extern "C" void wtc_c_queue_test_hook(const char* where);
#define WTC_C_QUEUE_HOOK(where) wtc_c_queue_test_hook(where)
#else
#define WTC_C_QUEUE_HOOK(where) ((void)0)
#endif

struct c_queue {
    c_queue(topology tag, std::size_t capacity, int producers, int ends)
        : ring(capacity, manyProducers(tag), manyConsumers(tag), /*overrun=*/false, /*batches=*/true),
          not_empty(wtc::rt::wait_policy::spin_then_park),
          not_full(wtc::rt::wait_policy::spin_then_park),
          producers(producers),
          ends(ends) {
        if (ends != ends_never && producers <= 0) not_empty.close();
    }

    wtc::rt::bounded_ring<void*> ring;
    // Their own lines: a waiter that goes to sleep or wakes writes them.
    alignas(128) wtc::rt::waiter not_empty;
    alignas(128) wtc::rt::waiter not_full;
    alignas(128) std::atomic<int> ended{0};
    // Operations under way, each side's on its own line: destroy waits for
    // them (see `active` below).
    alignas(128) std::atomic<int> pushing{0};
    alignas(128) std::atomic<int> popping{0};
    const int producers;
    const int ends;
};

// An operation counted from before it makes anything visible until it no
// longer touches the queue. The original's operations hold the mutex until
// they are done with the queue, so a thread that has seen their effect
// (taken the element, seen the queue finished) may destroy it at once:
// pthread_mutex_destroy of an unlocked mutex and pthread_cond_destroy of a
// variable nobody waits on are allowed. The substitute's still wake
// sleepers after their effect is visible (waiter.hpp, "Lifetime"), and
// destroy waits until none is under way. The count is taken before the
// effect, so whoever has seen the effect sees it counted until it ends.
struct active {
    std::atomic<int>& n;
    explicit active(std::atomic<int>& n) : n(n) { n.fetch_add(1, std::memory_order_relaxed); }
    ~active() { n.fetch_sub(1, std::memory_order_release); }
    active(const active&) = delete;
    active& operator=(const active&) = delete;
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

unsigned long least(unsigned long a, unsigned long limit) { return a < limit ? a : limit; }

}  // namespace

extern "C" {

void wtc_c_queue_init(void* at, int tag, long capacity, int producers, int ends) {
    if (capacity < 0) fail("built with a negative capacity", at);
    if (ends == ends_exactly && producers < 0) fail("built to finish after a negative number of producers", at);
    c_queue* q = new c_queue(static_cast<topology>(tag), static_cast<std::size_t>(capacity), producers, ends);
    std::memcpy(at, &q, sizeof q);
}

void wtc_c_queue_destroy(void* at) {
    c_queue* q = get(at);
    while (q->pushing.load(std::memory_order_acquire) != 0 || q->popping.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    delete q;
}

void wtc_c_queue_push_wait(void* at, void* v) {
    c_queue* q = get(at);
    active a(q->pushing);
    q->not_full.wait([&]() noexcept { return q->ring.try_push(v); });
    WTC_C_QUEUE_HOOK("push");
    q->not_empty.notify(1);
}

int wtc_c_queue_pop_wait(void* at, void** out) {
    c_queue* q = get(at);
    active a(q->popping);
    if (!q->not_empty.wait([&]() noexcept { return q->ring.consume([&](void*&& e) noexcept { *out = e; }); }))
        return 0;
    WTC_C_QUEUE_HOOK("pop");
    q->not_full.notify(1);
    return 1;
}

void wtc_c_queue_terminate(void* at) {
    c_queue* q = get(at);
    active a(q->pushing);
    if (q->ends == ends_never) fail("terminated, but built without a number of producers", at);
    const int n = q->ended.fetch_add(1, std::memory_order_seq_cst) + 1;
    if (n > q->producers) {
        if (q->ends == ends_exactly) fail("terminated more often than the producers it was built for", at);
        return;  // ends_at_least: finished already, and stays so
    }
    if (n == q->producers) {
        q->not_empty.close();
        WTC_C_QUEUE_HOOK("terminate");
    }
}

int wtc_c_queue_push_batch(void* at, void*** data, int* in, int* out, unsigned long slots, unsigned long limit) {
    c_queue* q = get(at);
    active a(q->pushing);
    own_ring src{*data, in, out, slots};
    const unsigned long n = least(src.count(), limit);
    if (n == 0) {
        q->not_full.wait([&]() noexcept { return q->ring.size() < q->ring.capacity(); });
        // The original moves nothing and returns. A wake-up meant for a
        // push with something to move may have come here instead: passed
        // on, so that a push the original's wake-ups would let through is
        // not left asleep with room in the queue.
        q->not_full.notify(1);
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

int wtc_c_queue_pop_batch(void* at, void*** data, int* in, int* out, unsigned long slots, unsigned long limit) {
    c_queue* q = get(at);
    active a(q->popping);
    own_ring dst{*data, in, out, slots};
    const unsigned long n = least(dst.room(), limit);
    if (n == 0) {
        if (!q->not_empty.wait([&]() noexcept { return q->ring.size() > 0; })) return -1;
        q->not_empty.notify(1);  // as for the push above
        return 0;
    }
    std::size_t k = 0;
    if (!q->not_empty.wait([&]() noexcept {
            k = q->ring.consume_n(n, [&](void*&& e) noexcept { dst.insert(e); });
            return k > 0;
        }))
        return -1;
    q->not_full.notify(k);
    return static_cast<int>(k);
}

}  // extern "C"
