// Two threads and one queue of a move-only element, popped by moving it
// into the consumer's own object: try_pop(T&), the transfer form. The
// element cannot be copied, so the program could not have used the
// shared_ptr form's push by copy, and nothing is allocated for it on
// either side: the push moves it into the queue's cell and the pop moves
// it out. The consumer's step is declared lock-free; it passes on the spsc
// ring and fails on the general-purpose queue (-DGENERIC, via
// --no-specialize), whose pop frees the segments it moves past.

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>
#include <cstdio>
#include <utility>

// Move-only, trivially destructible: a handle rather than a value.
struct token {
    std::uint64_t seq;
    std::uint64_t pad[7];
    token() = default;
    token(const token&) = delete;
    token& operator=(const token&) = delete;
    token(token&&) = default;
    token& operator=(token&&) = default;
};

wtc::queue<token> q;

constexpr std::uint64_t kCount = 200000;
std::uint64_t sum = 0;
bool in_order = true;

void produce() {
    for (std::uint64_t i = 0; i < kCount; ++i) {
        token t;
        t.seq = i;
        t.pad[0] = i * 3;
        q.push(std::move(t));
    }
}

WTC_LOCK_FREE bool consume_one(token& t) { return q.try_pop(t); }

void consume() {
    token t;
    for (std::uint64_t got = 0; got < kCount;) {
        if (!consume_one(t)) continue;
        if (t.seq != got) in_order = false;
        sum += t.pad[0];
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
