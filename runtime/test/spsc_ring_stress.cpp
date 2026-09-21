// Two-thread stress test for wtc::rt::spsc_ring. The consumer checks that
// every element arrives exactly once and in order; the producer varies its
// burst length so block turnovers land at every phase relative to the
// consumer. Run it under ThreadSanitizer as well: with a correct ring TSan
// is silent, and with a second producer (WTC_STRESS_TWO_PRODUCERS) it must
// report a race, which is the test that the test can fail.

#include "spsc_ring.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

struct payload {
  std::uint64_t seq;
  std::uint64_t pad[7];
};

using ring = wtc::rt::spsc_ring<std::shared_ptr<payload>, 64>;

std::uint64_t parse_count(int argc, char** argv, std::uint64_t dflt) {
  return argc > 1 ? std::strtoull(argv[1], nullptr, 10) : dflt;
}

} // namespace

int main(int argc, char** argv) {
  const std::uint64_t n = parse_count(argc, argv, 5'000'000);
  ring q;
  std::atomic<bool> failed{false};

  auto producer = [&](std::uint64_t first, std::uint64_t step) {
    unsigned burst = 1;
    for (std::uint64_t i = first; i < n; i += step) {
      auto p = std::make_shared<payload>();
      p->seq = i;
      q.push(std::move(p));
      if (--burst == 0) {
        burst = 1 + static_cast<unsigned>(i % 97);
        std::this_thread::yield();
      }
    }
  };

  const auto t0 = std::chrono::steady_clock::now();
#ifdef WTC_STRESS_TWO_PRODUCERS
  std::thread pa(producer, 0, 2), pb(producer, 1, 2);
#else
  std::thread pa(producer, 0, 1);
#endif

  std::uint64_t expect = 0;
  std::uint64_t spins = 0;
  // A broken ring can lose elements, and then the count below never arrives.
  // Bound the wait by progress, not by total time, so a slow sanitizer run
  // is not mistaken for a stall.
  auto last_progress = std::chrono::steady_clock::now();
  while (expect < n) {
    std::shared_ptr<payload> p;
    if (!q.try_pop(p)) {
      if (++spins % 4096 == 0) {
        std::this_thread::yield();
        if (std::chrono::steady_clock::now() - last_progress >
            std::chrono::seconds(10)) {
          std::fprintf(stderr, "stalled: %llu of %llu elements arrived\n",
                       static_cast<unsigned long long>(expect),
                       static_cast<unsigned long long>(n));
          failed = true;
          break;
        }
      }
      continue;
    }
    last_progress = std::chrono::steady_clock::now();
#ifndef WTC_STRESS_TWO_PRODUCERS
    if (p->seq != expect) {
      std::fprintf(stderr, "order broken: got %llu, expected %llu\n",
                   static_cast<unsigned long long>(p->seq),
                   static_cast<unsigned long long>(expect));
      failed = true;
      break;
    }
#endif
    ++expect;
  }
  pa.join();
#ifdef WTC_STRESS_TWO_PRODUCERS
  pb.join();
#endif
  const auto dt = std::chrono::steady_clock::now() - t0;
  const double s = std::chrono::duration<double>(dt).count();

  std::shared_ptr<payload> leftover;
  if (!failed && q.try_pop(leftover)) {
    std::fprintf(stderr, "queue not empty after %llu pops\n",
                 static_cast<unsigned long long>(n));
    failed = true;
  }
  if (failed) return 1;
  std::printf("spsc_ring_stress: ok, %llu elements in %.2f s (%.1f M/s)\n",
              static_cast<unsigned long long>(n), s, n / s / 1e6);
  return 0;
}
