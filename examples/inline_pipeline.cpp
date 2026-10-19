// Two threads and one queue whose elements are held inline in the ring. The
// consumer only reads each element it pops before its next pop, which is
// the contract spsc_inline needs. With -DDECLARE_INLINE the program sets
// that tag itself; otherwise the compiler decides.

#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>
#include <cstdio>

struct record {
    std::uint64_t seq;
    std::uint64_t pad[7];
};

#ifdef DECLARE_INLINE
wtc::queue<record> q(wtc::topology::spsc_inline);
#else
wtc::queue<record> q;
#endif

constexpr std::uint64_t kCount = 200000;
std::uint64_t sum = 0;
bool in_order = true;

void produce() {
    for (std::uint64_t i = 0; i < kCount; ++i) {
        record r{};
        r.seq = i;
        r.pad[0] = i * 3;
        q.push(r);
    }
}

void consume() {
    for (std::uint64_t got = 0; got < kCount;) {
        auto p = q.try_pop();
        if (!p) continue;
        if (p->seq != got) in_order = false;
        sum += p->pad[0];
        ++got;
    }
}

int main() {
    std::thread a = wtc::spawn(produce);
    std::thread b = wtc::spawn(consume);
    a.join();
    b.join();
    const char* tag = q.tag() == wtc::topology::spsc_inline ? "spsc_inline"
                      : q.tag() == wtc::topology::spsc    ? "spsc"
                                                          : "generic";
    std::printf("tag %s in_order %d sum %llu\n", tag, in_order ? 1 : 0,
                static_cast<unsigned long long>(sum));
    return in_order ? 0 : 1;
}
