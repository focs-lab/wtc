// Stress test for wtc::rt::segment_queue. Four producers, one of them in
// runs of 1 to 97 taken with one fetch-and-add each, and four consumers, or
// one consumer with WTC_STRESS_ONE_CONSUMER (the single-consumer form). The
// segments hold 64 cells, so they turn over every few dozen elements and
// the hazard pointers retire and free them all the time. Every element
// must arrive exactly once, and since the queue is FIFO per producer, each
// consumer must see each producer's elements in increasing order. Under
// ThreadSanitizer a correct queue is silent. With WTC_STRESS_TWO_CONSUMERS
// the single-consumer form gets a second consumer, which WTC_CHECK must
// abort and TSan must report: the test that the test can fail. With
// WTC_STRESS_STALL the producers now and then stop between taking a cell
// and filling it, and the single consumer checks every empty answer: it
// must have popped every element whose push had finished when it asked.
// The single consumer also checks real-time order across producers: an
// element whose push finished before another's push began (as a counter
// every push bumps when it finishes shows) must come out first.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "segment_queue.hpp"

namespace {
#ifdef WTC_STRESS_STALL
thread_local unsigned g_stall_seed = 1;
// Now and then hold a push between taking its cell and filling it.
struct stall_hooks : wtc::rt::no_hooks {
  static void reserved() {
    g_stall_seed = g_stall_seed * 1103515245u + 12345u;
    if ((g_stall_seed >> 16) % 256 == 0) std::this_thread::sleep_for(std::chrono::microseconds(20));
  }
};
using hooks = stall_hooks;
#else
using hooks = wtc::rt::no_hooks;
#endif
}  // namespace

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
using queue = wtc::rt::segment_queue<std::shared_ptr<payload>, kMany, 64, 64, hooks>;
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

  // How many pushes each producer has finished, for the empty-answer check.
  std::atomic<std::uint64_t> finished[kProducers] = {};
  std::atomic<bool> empty_ok{true};
  // For the real-time check: per element, how many pushes had finished when
  // its push began, its rank among finished pushes, and its rank among pops.
  std::atomic<std::uint64_t> completions{0};
  std::vector<std::uint64_t> started(kProducers * n), completed(kProducers * n), popped(kProducers * n);

  std::vector<std::thread> threads;
  for (unsigned p = 0; p < kProducers; ++p) {
    threads.emplace_back([&, p, n] {
#ifdef WTC_STRESS_STALL
      g_stall_seed = 2 * p + 1;
#endif
      if (p == 0) {
        std::vector<std::shared_ptr<payload>> run;
        unsigned len = 1;
        for (std::uint64_t i = 0; i < n;) {
          run.clear();
          std::uint64_t first = i, before = completions.load();
          for (unsigned k = 0; k < len && i < n; ++k, ++i) {
            run.push_back(std::make_shared<payload>(payload{p, i}));
            started[p * n + i] = before;
          }
          q.push_run(run.data(), run.size());
          std::uint64_t rank = completions.fetch_add(i - first);
          for (std::uint64_t j = first; j < i; ++j) completed[p * n + j] = rank + (j - first);
          finished[p].store(i, std::memory_order_release);
          len = len % 97 + 1;
        }
        return;
      }
      for (std::uint64_t i = 0; i < n; ++i) {
        started[p * n + i] = completions.load();
        q.push(std::make_shared<payload>(payload{p, i}));
        completed[p * n + i] = completions.fetch_add(1);
        finished[p].store(i + 1, std::memory_order_release);
      }
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
      std::uint64_t due = 0;
      if (kConsumers == 1)
        for (auto& f : finished) due += f.load(std::memory_order_acquire);
      if (!q.try_pop(e)) {
        // The only consumer answered empty: everything whose push had
        // finished before it asked must already be popped.
        if (kConsumers == 1 && got.load(std::memory_order_relaxed) < due) empty_ok.store(false);
        if (producers_done.load(std::memory_order_acquire)) break;
        continue;
      }
      if (e->seq < next[e->from]) ok.store(false);
      next[e->from] = e->seq + 1;
      if (seen[e->from * n + e->seq].exchange(1)) ok.store(false);
      popped[e->from * n + e->seq] = got.fetch_add(1, std::memory_order_relaxed);
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
  if (kConsumers == 1 && got.load() == until) {
    // Every element whose push finished before Y's began must have been
    // popped before Y: the latest pop among the first started[Y] finished
    // pushes comes before Y's pop.
    std::vector<std::uint64_t> by_rank(until), latest(until + 1, 0);
    for (std::uint64_t id = 0; id < until; ++id) by_rank[completed[id]] = id;
    std::uint64_t m = 0;
    for (std::uint64_t r = 0; r < until; ++r) {
      latest[r] = m;
      m = std::max(m, popped[by_rank[r]] + 1);
    }
    latest[until] = m;
    for (std::uint64_t id = 0; id < until; ++id) {
      if (latest[started[id]] > popped[id]) {
        std::fprintf(stderr, "segment_queue_stress: an element came out before one whose push finished first\n");
        return 1;
      }
    }
  }
  if (!empty_ok.load()) {
    std::fprintf(stderr, "segment_queue_stress: empty while a finished push's element was queued\n");
    return 1;
  }
  if (!ok.load()) {
    std::fprintf(stderr, "segment_queue_stress: an element arrived twice or out of order\n");
    return 1;
  }
  std::printf("segment_queue_stress: %llu elements from %u producers to %u consumers, each in order\n",
              static_cast<unsigned long long>(got.load()), kProducers, kConsumers);
  return 0;
}
