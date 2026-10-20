// Stress test for wtc::rt::mpsc_list: four producers, one consumer. The
// consumer checks that every element arrives exactly once and that each
// producer's arrive in the order it pushed them; one producer pushes in
// runs of varying length, spliced in with one exchange each, so splices
// land at every phase relative to the others. Under ThreadSanitizer a
// correct list is silent. With WTC_STRESS_TWO_CONSUMERS a second thread
// pops as well, which WTC_CHECK must abort and TSan must report: the test
// that the test can fail.

#include "mpsc_list.hpp"

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

using list = wtc::rt::mpsc_list<std::shared_ptr<payload>>;
constexpr unsigned kProducers = 4;

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  list q;
  std::vector<std::thread> producers;
  for (unsigned p = 0; p < kProducers; ++p) {
    producers.emplace_back([&q, p, n] {
      if (p == 0) {
        // Runs of 1 to 97 pushes, published together.
        list::chain c;
        unsigned run = 1;
        for (std::uint64_t i = 0; i < n;) {
          for (unsigned k = 0; k < run && i < n; ++k, ++i)
            q.push_deferred(c, std::make_shared<payload>(payload{p, i}));
          q.publish(c);
          run = run % 97 + 1;
        }
        return;
      }
      for (std::uint64_t i = 0; i < n; ++i) q.push(std::make_shared<payload>(payload{p, i}));
    });
  }
  std::uint64_t next[kProducers] = {};
  std::uint64_t got = 0;
  bool ok = true;
  auto consume = [&](std::uint64_t until) {
    std::shared_ptr<payload> e;
    while (got < until) {
      if (!q.try_pop(e)) continue;
      if (e->seq != next[e->from]++) ok = false;
      ++got;
    }
  };
#ifdef WTC_STRESS_TWO_CONSUMERS
  std::thread second([&q] {
    std::shared_ptr<payload> e;
    for (int i = 0; i < 100000; ++i) q.try_pop(e);
  });
  consume(kProducers * n / 2);
  second.join();
#else
  consume(kProducers * n);
#endif
  for (auto& t : producers) t.join();
  if (!ok) {
    std::fprintf(stderr, "mpsc_list_stress: a producer's elements arrived out of order\n");
    return 1;
  }
  std::printf("mpsc_list_stress: %llu elements from %u producers, each in order\n",
              static_cast<unsigned long long>(got), kProducers);
  return 0;
}
