// A loop that pushes a move-only element: the compiler batches it through
// the moving deferred push, so nothing is copied from an object the
// program has given up, and every element arrives once and in order.

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>
#include <cstdio>
#include <utility>

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
constexpr std::uint64_t kCount = 100000;

WTC_BATCH void produce() {
    for (std::uint64_t i = 0; i < kCount; ++i) {
        token t;
        t.seq = i;
        q.push(std::move(t));
    }
}

int main() {
    std::thread a = wtc::spawn(produce);
    std::uint64_t got = 0;
    bool in_order = true;
    token t;
    while (got < kCount) {
        if (!q.try_pop(t)) continue;
        if (t.seq != got) in_order = false;
        ++got;
    }
    a.join();
    std::printf("in_order %d got %llu\n", in_order ? 1 : 0, static_cast<unsigned long long>(got));
    return in_order ? 0 : 1;
}
