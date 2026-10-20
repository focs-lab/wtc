// Stress test for wtc::rt::spmc_ring: one producer, four consumers. The
// producer pushes in runs of 1 to 97, a run of one by push and a longer one
// published with one store, so publications land at every phase relative
// to the consumers, and the ring is small enough to turn over blocks and
// grow while they read. Every element must arrive exactly once, and since the order is
// the producer's, each consumer must see its own elements in increasing
// order. Under ThreadSanitizer a correct ring is silent. With
// WTC_STRESS_TWO_PRODUCERS a second thread pushes as well, which WTC_CHECK
// must abort and TSan must report: the test that the test can fail.

#include "spmc_ring.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

struct payload {
  std::uint64_t seq;
};

using ring = wtc::rt::spmc_ring<std::shared_ptr<payload>, 64>;
constexpr unsigned kConsumers = 4;

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  ring q;
  std::atomic<bool> done{false};
  auto produce = [&q](std::uint64_t from, std::uint64_t to) {
    unsigned run = 1;
    for (std::uint64_t i = from; i < to;) {
      if (run == 1) {
        q.push(std::make_shared<payload>(payload{i++}));
      } else {
        for (unsigned k = 0; k < run && i < to; ++k, ++i)
          q.push_deferred(std::make_shared<payload>(payload{i}));
        q.publish();
      }
      run = run % 97 + 1;
    }
  };

  std::vector<std::atomic<unsigned char>> seen(n);
  std::atomic<std::uint64_t> got{0};
  std::atomic<bool> ok{true};
  std::vector<std::thread> consumers;
  for (unsigned c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&] {
      std::shared_ptr<payload> e;
      std::uint64_t last = 0;
      bool any = false;
      for (;;) {
        if (!q.try_pop(e)) {
          if (!done.load(std::memory_order_acquire)) continue;
          if (!q.try_pop(e)) break;
        }
        const std::uint64_t s = e->seq;
        if ((any && s <= last) || s >= n || seen[s].exchange(1, std::memory_order_relaxed))
          ok.store(false, std::memory_order_relaxed);
        last = s;
        any = true;
        got.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

#ifdef WTC_STRESS_TWO_PRODUCERS
  std::thread second([&] { produce(n / 2, n); });
  produce(0, n / 2);
  second.join();
#else
  produce(0, n);
#endif
  done.store(true, std::memory_order_release);
  for (auto& t : consumers) t.join();

  if (!ok.load() || got.load() != n) {
    std::fprintf(stderr,
                 "spmc_ring_stress: %llu of %llu elements arrived, or one twice or out of order\n",
                 static_cast<unsigned long long>(got.load()), static_cast<unsigned long long>(n));
    return 1;
  }
  std::printf("spmc_ring_stress: %llu elements to %u consumers, each once and in order\n",
              static_cast<unsigned long long>(n), kConsumers);
  return 0;
}
