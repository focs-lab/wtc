// Stress test for wtc::rt::segment_queue. Four producers, one of them in
// runs of 1 to 97 taken with one fetch-and-add each, and four consumers, or
// one consumer with WTC_STRESS_ONE_CONSUMER (the single-consumer form). The
// segments hold 64 cells, so they turn over every few dozen elements and
// the hazard pointers retire and free them all the time. Every element
// must arrive exactly once, and since the queue is FIFO per producer, each
// consumer must see each producer's elements in increasing order. Under
// ThreadSanitizer a correct queue is silent. With WTC_STRESS_TWO_CONSUMERS
// the single-consumer form gets a second consumer, which WTC_CHECK must
// abort and TSan must report: the test that the test can fail.

#include "segment_queue.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

struct payload {
  std::uint32_t from;
  std::uint64_t seq;
};

#if defined(WTC_STRESS_ONE_CONSUMER) || defined(WTC_STRESS_TWO_CONSUMERS)
constexpr bool kMany = false;
constexpr unsigned kConsumers = 1;
#else
constexpr bool kMany = true;
constexpr unsigned kConsumers = 4;
#endif
using queue = wtc::rt::segment_queue<std::shared_ptr<payload>, kMany, 64>;
constexpr unsigned kProducers = 4;

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  queue q;
  // One flag per element: set by whichever consumer takes it.
  std::vector<std::atomic<unsigned char>> seen(kProducers * n);
  for (auto& s : seen) s.store(0, std::memory_order_relaxed);
  std::atomic<std::uint64_t> got{0};
  std::atomic<bool> ok{true};

  std::vector<std::thread> threads;
  for (unsigned p = 0; p < kProducers; ++p) {
    threads.emplace_back([&q, p, n] {
      if (p == 0) {
        std::vector<std::shared_ptr<payload>> run;
        unsigned len = 1;
        for (std::uint64_t i = 0; i < n;) {
          run.clear();
          for (unsigned k = 0; k < len && i < n; ++k, ++i)
            run.push_back(std::make_shared<payload>(payload{p, i}));
          q.push_run(run.data(), run.size());
          len = len % 97 + 1;
        }
        return;
      }
      for (std::uint64_t i = 0; i < n; ++i) q.push(std::make_shared<payload>(payload{p, i}));
    });
  }
#ifdef WTC_STRESS_TWO_CONSUMERS
  // The second consumer takes some, so the first stops at half.
  const std::uint64_t until = kProducers * n / 2;
#else
  const std::uint64_t until = kProducers * n;
#endif
  // A lost element would leave the consumers waiting for ever: once every
  // producer has returned, an empty pop ends a consumer, and the count is
  // checked after the join.
  std::atomic<bool> producers_done{false};
  auto consume = [&] {
    std::uint64_t next[kProducers] = {};
    std::shared_ptr<payload> e;
    while (got.load(std::memory_order_relaxed) < until) {
      if (!q.try_pop(e)) {
        if (producers_done.load(std::memory_order_acquire)) break;
        continue;
      }
      if (e->seq < next[e->from]) ok.store(false);
      next[e->from] = e->seq + 1;
      if (seen[e->from * n + e->seq].exchange(1)) ok.store(false);
      got.fetch_add(1, std::memory_order_relaxed);
    }
  };
  for (unsigned c = 0; c < kConsumers; ++c) threads.emplace_back(consume);
#ifdef WTC_STRESS_TWO_CONSUMERS
  threads.emplace_back([&q] {
    std::shared_ptr<payload> e;
    for (int i = 0; i < 100000; ++i) q.try_pop(e);
  });
#endif
  for (unsigned i = 0; i < kProducers; ++i) threads[i].join();
  producers_done.store(true, std::memory_order_release);
  for (unsigned i = kProducers; i < threads.size(); ++i) threads[i].join();
#ifndef WTC_STRESS_TWO_CONSUMERS
  if (got.load() != until) {
    std::fprintf(stderr, "segment_queue_stress: %llu of %llu elements arrived\n",
                 static_cast<unsigned long long>(got.load()), static_cast<unsigned long long>(until));
    return 1;
  }
#endif
  if (!ok.load()) {
    std::fprintf(stderr, "segment_queue_stress: an element arrived twice or out of order\n");
    return 1;
  }
  std::printf("segment_queue_stress: %llu elements from %u producers to %u consumers, each in order\n",
              static_cast<unsigned long long>(got.load()), kProducers, kConsumers);
  return 0;
}
