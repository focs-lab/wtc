// Stress test for wtc::rt::spmc_queue: one producer and 1, 2 or 8
// consumers (the second argument, 4 by default). The producer pushes in
// runs of 1 to 97, a run of one by push and a longer one published with one
// store, so publications land at every phase relative to the consumers, and
// the segments are small enough that runs cross them and the head moves on
// and retires segments while consumers read. Every element must arrive
// exactly once, and since the order is the producer's, each consumer must
// see its own elements in increasing order. The element is a shared_ptr, so
// a cell read before its element was published, or after its segment was
// freed, shows up as a bad sequence number or under ThreadSanitizer. With
// WTC_STRESS_TWO_PRODUCERS a second thread pushes as well, which WTC_CHECK
// must abort and TSan must report: the test that the test can fail.
//
// With WTC_STRESS_DTOR_PUSH the element's destructor pushes its sequence
// number to a general-purpose queue. It runs inside the pop's taker (the
// consumer's previous element dies when the next one is moved over it),
// while the pop still needs its segment, and that push protects and clears
// a hazard slot of its own: a pop that shared the push's slot would lose
// its protection there, and a segment another consumer retires meanwhile
// would be freed under it, which AddressSanitizer reports. Every element
// must also be destroyed exactly once.

#include "segment_queue.hpp"
#include "spmc_queue.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

#ifdef WTC_STRESS_DTOR_PUSH
wtc::rt::segment_queue<std::uint64_t, true, 64> g_destroyed;
#endif

struct payload {
  std::uint64_t seq;
  // Built in place by make_shared: no temporary whose destructor would
  // count as the element's.
  explicit payload(std::uint64_t s) : seq(s) {}
#ifdef WTC_STRESS_DTOR_PUSH
  ~payload() { g_destroyed.push(seq); }
#endif
};

using queue = wtc::rt::spmc_queue<std::shared_ptr<payload>, 64>;

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
  const unsigned consumers_n = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 4;
  queue q;
  std::atomic<bool> done{false};
  auto produce = [&q](std::uint64_t from, std::uint64_t to) {
    unsigned run = 1;
    for (std::uint64_t i = from; i < to;) {
      if (run == 1) {
        q.push(std::make_shared<payload>(i++));
      } else {
        for (unsigned k = 0; k < run && i < to; ++k, ++i)
          q.push_deferred(std::make_shared<payload>(i));
        q.publish();
      }
      run = run % 97 + 1;
    }
  };

  std::vector<std::atomic<unsigned char>> seen(n);
  std::atomic<std::uint64_t> got{0};
  std::atomic<bool> ok{true};
  std::vector<std::thread> consumers;
  for (unsigned c = 0; c < consumers_n; ++c) {
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

#ifdef WTC_STRESS_DTOR_PUSH
  // Every consumer has let its last element go; each element was destroyed,
  // and pushed its number, exactly once.
  std::vector<unsigned char> destroyed(n, 0);
  std::uint64_t d = 0, count = 0;
  while (g_destroyed.try_pop(d)) {
    if (d >= n || destroyed[d]++) ok.store(false, std::memory_order_relaxed);
    ++count;
  }
  if (count != n) ok.store(false, std::memory_order_relaxed);
#endif
  if (!ok.load() || got.load() != n) {
    std::fprintf(stderr,
                 "spmc_queue_stress: %llu of %llu elements arrived, or one twice or out of order\n",
                 static_cast<unsigned long long>(got.load()), static_cast<unsigned long long>(n));
    return 1;
  }
  std::printf("spmc_queue_stress: %llu elements to %u consumers, each once and in order\n",
              static_cast<unsigned long long>(n), consumers_n);
  return 0;
}
