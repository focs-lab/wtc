#pragma once

#include <cstdint>

// Element types for the workloads. Each carries the sequence number the
// consumer checks and, for latency workloads, two timestamps: when the
// element was due to be sent and when it was sent. Latency is measured from
// the first, so an element delayed by a producer that fell behind its
// schedule is charged that delay rather than hidden (coordinated omission);
// the second gives the queue's own share. The rest is padding to the stated
// size, which is what the allocator and the copy see. The send time is kept
// in the first padding word, so the layout, and the work the pipeline does
// over the padding, are what they were; only W2 sets it, and W2 does not
// read the padding.
namespace wtc_bench {

struct E8 {
    std::uint64_t seq;
    std::uint64_t tsc() const { return 0; }
    void set_tsc(std::uint64_t) {}
    std::uint64_t sent() const { return 0; }
    void set_sent(std::uint64_t) {}
};

struct E64 {
    std::uint64_t seq;
    std::uint64_t stamp;
    std::uint64_t pad[6];
    std::uint64_t tsc() const { return stamp; }
    void set_tsc(std::uint64_t t) { stamp = t; }
    std::uint64_t sent() const { return pad[0]; }
    void set_sent(std::uint64_t t) { pad[0] = t; }
};

struct E256 {
    std::uint64_t seq;
    std::uint64_t stamp;
    std::uint64_t pad[30];
    std::uint64_t tsc() const { return stamp; }
    void set_tsc(std::uint64_t t) { stamp = t; }
    std::uint64_t sent() const { return pad[0]; }
    void set_sent(std::uint64_t t) { pad[0] = t; }
};

static_assert(sizeof(E8) == 8);
static_assert(sizeof(E64) == 64);
static_assert(sizeof(E256) == 256);

}
