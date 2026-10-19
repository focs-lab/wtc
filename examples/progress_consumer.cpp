// A consumer whose per-element step is declared lock-free. Whether the
// promise holds depends on what the compiler chooses for the queue and on
// what the step does with the element:
//
//   wtc-cc progress_consumer.cpp                 passes: the queue is spsc,
//       its consumer only borrows, so its elements are held inline and the
//       pop neither locks nor frees
//   wtc-cc --no-specialize progress_consumer.cpp fails: the general-purpose
//       pop takes the queue's mutexes
//   wtc-cc -DRETAIN progress_consumer.cpp        fails: keeping the element
//       means it is not borrowed, so it lives in a shared_ptr whose release
//       may free it
//   wtc-cc -DLOG progress_consumer.cpp           fails: printf
//
// The producer's side never passes, and -DPRODUCER shows it: each push may
// find its block full and allocate the next one, since the queue has no
// capacity to run out of.
//
//   wtc-cc -DPRODUCER progress_consumer.cpp      fails: the push may
//       allocate a block

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>

struct sample {
    std::uint64_t seq;
    double value;
};

wtc::queue<sample> q;
std::uint64_t total = 0;
#ifdef RETAIN
std::shared_ptr<sample> last;
#endif

constexpr std::uint64_t kCount = 100000;

#ifdef PRODUCER
WTC_LOCK_FREE
#endif
void produce_one(std::uint64_t i) { q.push(sample{i, 0.5 * static_cast<double>(i)}); }

void produce() {
    for (std::uint64_t i = 0; i < kCount; ++i) produce_one(i);
}

WTC_LOCK_FREE bool consume_one() {
    auto p = q.try_pop();
    if (!p) return false;
#ifdef LOG
    std::printf("got %llu\n", static_cast<unsigned long long>(p->seq));
#endif
#ifdef RETAIN
    last = p;
#endif
    total += p->seq;
    return true;
}

void consume() {
    for (std::uint64_t got = 0; got < kCount;)
        if (consume_one()) ++got;
}

int main() {
    std::thread a = wtc::spawn(produce);
    std::thread b = wtc::spawn(consume);
    a.join();
    b.join();
    std::printf("total %llu\n", static_cast<unsigned long long>(total));
    return 0;
}
