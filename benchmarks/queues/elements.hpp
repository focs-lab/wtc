#pragma once

#include <cstdint>

// Element types for the workloads. Each carries the sequence number the
// consumer checks and the producer's timestamp for latency workloads; the
// rest is padding to the stated size, which is what the allocator and the
// copy see.
namespace wtc_bench {

struct E8 {
    std::uint64_t seq;
    std::uint64_t tsc() const { return 0; }
    void set_tsc(std::uint64_t) {}
};

struct E64 {
    std::uint64_t seq;
    std::uint64_t stamp;
    std::uint64_t pad[6];
    std::uint64_t tsc() const { return stamp; }
    void set_tsc(std::uint64_t t) { stamp = t; }
};

struct E256 {
    std::uint64_t seq;
    std::uint64_t stamp;
    std::uint64_t pad[30];
    std::uint64_t tsc() const { return stamp; }
    void set_tsc(std::uint64_t t) { stamp = t; }
};

static_assert(sizeof(E8) == 8);
static_assert(sizeof(E64) == 64);
static_assert(sizeof(E256) == 256);

}
