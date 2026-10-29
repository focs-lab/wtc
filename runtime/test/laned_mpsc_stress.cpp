// Stress test for wtc::rt::laned_mpsc. Six producers and one consumer:
// two push one element at a time, two push runs of 1 to 97 made visible
// with one publish, and two hand their work to a chain of short-lived
// threads, each of which pushes a few hundred elements and exits, so that
// lanes are made, drained and freed all through the run. The lanes' blocks
// hold 64 slots, so they turn over every few dozen elements. Every element
// must arrive exactly once, and each producer thread's elements in their
// order; the order between producers is not checked, because the queue
// does not keep it. Every empty answer is checked: everything whose push
// had finished when the consumer asked must have been popped. With
// WTC_STRESS_TWO_CONSUMERS a second consumer joins, which WTC_CHECK must
// abort and TSan must report: the test that the test can fail.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "laned_mpsc.hpp"

namespace {

struct payload {
  std::uint32_t from;  // the pushing thread's number, one per thread
  std::uint64_t seq;
};

using queue = wtc::rt::laned_mpsc<std::shared_ptr<payload>, 16, 64>;
constexpr unsigned kSteady = 4;     // producers 0-3: one by one, or in runs
constexpr unsigned kChains = 2;     // producers 4-5: chains of threads
constexpr unsigned kPerLink = 300;  // elements per thread of a chain

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 300'000;
  const std::uint64_t links = n / kPerLink;
  const std::uint64_t total = kSteady * n + kChains * links * kPerLink;
  const std::uint32_t threads_total = kSteady + kChains * static_cast<std::uint32_t>(links);

  queue q;
  // Pushes finished so far, for the empty-answer check.
  std::atomic<std::uint64_t> finished{0};
  std::atomic<bool> ok{true}, empty_ok{true};
  std::atomic<std::uint32_t> next_from{kSteady};

  std::vector<std::thread> threads;
  for (unsigned p = 0; p < kSteady; ++p) {
    threads.emplace_back([&, p] {
      if (p % 2 == 0) {
        for (std::uint64_t i = 0; i < n; ++i) {
          q.push(std::make_shared<payload>(payload{p, i}));
          finished.fetch_add(1, std::memory_order_release);
        }
        return;
      }
      unsigned len = 1;
      for (std::uint64_t i = 0; i < n;) {
        std::uint64_t first = i;
        for (unsigned k = 0; k < len && i < n; ++k, ++i)
          q.push_deferred(std::make_shared<payload>(payload{p, i}));
        q.publish();
        finished.fetch_add(i - first, std::memory_order_release);
        len = len % 97 + 1;
      }
    });
  }
  for (unsigned c = 0; c < kChains; ++c) {
    threads.emplace_back([&] {
      for (std::uint64_t l = 0; l < links; ++l) {
        std::thread link([&] {
          const std::uint32_t from = next_from.fetch_add(1);
          for (std::uint64_t i = 0; i < kPerLink; ++i) {
            q.push(std::make_shared<payload>(payload{from, i}));
            finished.fetch_add(1, std::memory_order_release);
          }
        });
        link.join();
      }
    });
  }

#ifdef WTC_STRESS_TWO_CONSUMERS
  // The second consumer takes some, so the first stops at half.
  const std::uint64_t until = total / 2;
  threads.emplace_back([&q] {
    std::shared_ptr<payload> e;
    for (int i = 0; i < 100000; ++i) q.try_pop(e);
  });
#else
  const std::uint64_t until = total;
#endif

  std::vector<std::uint64_t> next(threads_total, 0);
  std::uint64_t got = 0, polls = 0;
  std::shared_ptr<payload> e;
  while (got < until) {
    const std::uint64_t due = finished.load(std::memory_order_acquire);
    bool popped;
    if (++polls % 3 == 0)
      popped = q.consume([&](std::shared_ptr<payload>&& s) noexcept { e = std::move(s); });
    else
      popped = q.try_pop(e);
    if (!popped) {
      if (got < due) empty_ok.store(false);
      continue;
    }
    if (e->from >= threads_total || e->seq != next[e->from]) ok.store(false);
    else ++next[e->from];
    ++got;
  }
  for (auto& t : threads) t.join();

#ifndef WTC_STRESS_TWO_CONSUMERS
  std::shared_ptr<payload> extra;
  if (q.try_pop(extra)) ok.store(false);  // nothing beyond the total
  for (unsigned p = 0; p < kSteady; ++p)
    if (next[p] != n) ok.store(false);
  for (std::uint32_t f = kSteady; f < threads_total; ++f)
    if (next[f] != kPerLink) ok.store(false);
  // Every chain thread has exited and its lane is empty: the sweeps free
  // them all, and the steady producers' too.
  for (int i = 0; i < 1000; ++i) q.try_pop(extra);
  if (q.lanes() != 0 || q.retired() != threads_total) {
    std::fprintf(stderr, "lanes left %zu, freed %llu of %u\n", q.lanes(),
                 static_cast<unsigned long long>(q.retired()), threads_total);
    ok.store(false);
  }
#endif
  if (!ok.load()) {
    std::fprintf(stderr, "laned_mpsc_stress: lost, duplicated or reordered elements\n");
    return 1;
  }
  if (!empty_ok.load()) {
    std::fprintf(stderr, "laned_mpsc_stress: empty answered with a finished push unpopped\n");
    return 1;
  }
  std::printf("laned_mpsc_stress: ok, %llu elements from %u threads\n",
              static_cast<unsigned long long>(got), threads_total);
  return 0;
}
