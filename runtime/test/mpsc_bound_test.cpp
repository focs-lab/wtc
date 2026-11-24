// The single consumer's pop (segment_queue.hpp, single-consumer form) is
// bounded in its own steps by the reservations in flight, not by the
// queue's history.
//
//   late_fill   a push held between taking its cell and filling it while
//               another producer pushes N elements, which the consumer
//               takes out of order; then the held push fills. Every pop
//               from then on, the one that steps past the N taken cells
//               included, makes at most kMaxLooks looks and retires at most
//               one segment, for N from 10^3 to 4·10^6, and its time does
//               not grow with N. Until the head moved past taken cells at
//               once, the pop after the late fill stepped over all N of
//               them and their segments in one call: 11.5 ms at N = 8·10^6.
//   two_holes   two pushes held far apart, the cells between them taken:
//               the head moves from the first hole to the second when the
//               first is filled, and past both when the second is.
//   holes P     P producers pausing at random between taking a cell and
//               filling it: looks per pop stay within a multiple of P,
//               whatever the number of elements.
//   push_steps P  (mpsc_jiffy_bound_test, the one-counter push) P
//               producers pushing single elements and runs of up to 8
//               into segments of 64 cells, pausing at random after taking
//               their index: every element once and in its producer's
//               order, and the segments a push moves through to its
//               index's, forward and back, counted. Forward steps are
//               bounded by the pushes in flight, at most 1 + P * 8 / 64
//               segments; steps back by the segments linked while a push
//               was preempted, which the pauses make happen.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include "segment_queue.hpp"

// Under a sanitizer a pop's time is the sanitizer's: only the steps are
// asserted there.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define WTC_TEST_UNTIMED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define WTC_TEST_UNTIMED 1
#endif
#endif

namespace {
int failures = 0;

#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

// Hooks: a push of a thread with t_stall set holds after taking its cell
// until its gate opens; the consumer's looks and retires are counted.
struct gate {
  std::atomic<bool> at{false}, go{false};
};
thread_local gate* t_stall = nullptr;
thread_local unsigned t_jitter = 0;
thread_local std::uint64_t t_looks = 0, t_retires = 0;

thread_local std::uint64_t t_fwd = 0, t_back = 0;
struct hooks : wtc::rt::no_hooks {
  static void segment_step(int d) {
    if (d > 0) ++t_fwd;
    else ++t_back;
  }
  static void reserved() {
    if (gate* g = t_stall) {
      t_stall = nullptr;
      g->at.store(true);
      while (!g->go.load()) std::this_thread::yield();
    }
    if (t_jitter) {
      t_jitter = t_jitter * 1103515245u + 12345u;
      const unsigned r = (t_jitter >> 16) % 64;
      if (r == 0) std::this_thread::yield();
      else if (r < 8)
        for (unsigned i = 0; i < 200 * r; ++i) std::atomic_signal_fence(std::memory_order_seq_cst);
    }
  }
  static void look() { ++t_looks; }
  static void retire() { ++t_retires; }
};
// WTC_TEST_ONE_COUNTER: the one-counter push (the shim's jiffy).
#ifndef WTC_TEST_ONE_COUNTER
#define WTC_TEST_ONE_COUNTER 0
#endif
constexpr bool kOne = WTC_TEST_ONE_COUNTER;
using Q = wtc::rt::segment_queue<std::uint64_t, false, 1024, 64, hooks, kOne>;
using clk = std::chrono::steady_clock;

// Looks a pop may make with h holes: the walk's and the re-looks' of
// pop_ahead, O(h^2) with h at most 2 here.
constexpr std::uint64_t kMaxLooks = 8;

struct pop_cost {
  std::uint64_t looks = 0, retires = 0;
  double us = 0;
};

bool timed_pop(Q& q, std::uint64_t& v, pop_cost& worst) {
  t_looks = t_retires = 0;
  const auto t0 = clk::now();
  const bool got = q.try_pop(v);
  const double us = std::chrono::duration<double, std::micro>(clk::now() - t0).count();
  if (t_looks > worst.looks) worst.looks = t_looks;
  if (t_retires > worst.retires) worst.retires = t_retires;
  if (us > worst.us) worst.us = us;
  return got;
}

void late_fill() {
  double first_us = 0;
  for (std::uint64_t n : {std::uint64_t{1000}, std::uint64_t{100000}, std::uint64_t{1000000}, std::uint64_t{4000000}}) {
    Q q;
    gate g;
    std::thread stalled([&] {
      t_stall = &g;
      q.push(std::uint64_t{0});
    });
    while (!g.at.load()) std::this_thread::yield();
    std::thread([&] {
      for (std::uint64_t i = 1; i <= n; ++i) q.push(i);
    }).join();
    std::uint64_t v = 0, got = 0, expect = 1;
    pop_cost during;
    while (got < n) {
      if (timed_pop(q, v, during)) {
        EXPECT(v == expect);  // one producer's order, past the hole
        ++expect;
        ++got;
      }
    }
    g.go.store(true);
    stalled.join();
    // The late element, then empty: the pop that moves the head past the n
    // taken cells, and the ones after it that retire the segments.
    pop_cost after;
    EXPECT(timed_pop(q, v, after) && v == 0);
    for (int i = 0; i < 2000; ++i) EXPECT(!timed_pop(q, v, after));
    q.push(std::uint64_t{7});
    EXPECT(timed_pop(q, v, after) && v == 7);
    std::printf("late_fill n=%8llu: worst pop while holed: looks %llu retires %llu %.1f us; "
                "after the fill: looks %llu retires %llu %.1f us\n",
                (unsigned long long)n, (unsigned long long)during.looks,
                (unsigned long long)during.retires, during.us, (unsigned long long)after.looks,
                (unsigned long long)after.retires, after.us);
    EXPECT(during.looks <= kMaxLooks && after.looks <= kMaxLooks);
    EXPECT(during.retires <= 1 && after.retires <= 1);
    if (n == 1000) first_us = after.us;
#ifndef WTC_TEST_UNTIMED
    // Not linear in n: a thousandfold history may not cost a thousandfold
    // pop (it cost 0.6 ms at a million before the head moved at once). The
    // margin absorbs a preemption on a busy host.
    else EXPECT(after.us < 20 * first_us + 100);
#else
    (void)first_us;
#endif
  }
}

void two_holes() {
  Q q;
  gate g1, g2;
  std::thread a([&] {
    t_stall = &g1;
    q.push(std::uint64_t{1000000});
  });
  while (!g1.at.load()) std::this_thread::yield();
  std::thread([&] {
    for (std::uint64_t i = 0; i < 5000; ++i) q.push(i);
  }).join();
  std::thread b([&] {
    t_stall = &g2;
    q.push(std::uint64_t{2000000});
  });
  while (!g2.at.load()) std::this_thread::yield();
  std::thread([&] {
    for (std::uint64_t i = 5000; i < 10000; ++i) q.push(i);
  }).join();
  std::uint64_t v = 0, expect = 0;
  pop_cost cost;
  while (expect < 10000)
    if (timed_pop(q, v, cost)) EXPECT(v == expect++);
  EXPECT(!timed_pop(q, v, cost));
  g1.go.store(true);
  a.join();
  EXPECT(timed_pop(q, v, cost) && v == 1000000);  // the head moves on to the second hole
  for (int i = 0; i < 100; ++i) EXPECT(!timed_pop(q, v, cost));
  g2.go.store(true);
  b.join();
  EXPECT(timed_pop(q, v, cost) && v == 2000000);  // and past both
  for (int i = 0; i < 100; ++i) EXPECT(!timed_pop(q, v, cost));
  for (std::uint64_t i = 0; i < 3000; ++i) q.push(i);
  for (std::uint64_t i = 0; i < 3000; ++i) EXPECT(timed_pop(q, v, cost) && v == i);
  std::printf("two_holes: worst pop looks %llu retires %llu %.1f us\n", (unsigned long long)cost.looks,
              (unsigned long long)cost.retires, cost.us);
  EXPECT(cost.looks <= kMaxLooks);
  EXPECT(cost.retires <= 1);
}

void holes(unsigned producers, std::uint64_t total) {
  Q q;
  const std::uint64_t per = total / producers;
  std::atomic<bool> go{false};
  std::vector<std::thread> ts;
  for (unsigned k = 0; k < producers; ++k)
    ts.emplace_back([&, k] {
      t_jitter = 17u + k;
      while (!go.load()) {
      }
      for (std::uint64_t i = 0; i < per; ++i) q.push(std::uint64_t{k} << 40 | i);
    });
  go.store(true);
  std::vector<std::uint64_t> next(producers, 0);
  std::uint64_t got = 0, v = 0;
  pop_cost cost;
  while (got < per * producers) {
    if (timed_pop(q, v, cost)) {
      const unsigned k = static_cast<unsigned>(v >> 40);
      EXPECT((v & ((std::uint64_t{1} << 40) - 1)) == next[k]);
      ++next[k];
      ++got;
    }
  }
  for (auto& t : ts) t.join();
  std::printf("holes P=%2u: %llu elements, worst pop looks %llu retires %llu\n", producers,
              (unsigned long long)got, (unsigned long long)cost.looks, (unsigned long long)cost.retires);
  // h <= 2P holes, each looked at up to h times: O(P^2) in the worst case;
  // measured about 3.7 P. The bound asserted is the quadratic one.
  EXPECT(cost.looks <= 4ull * producers * producers + 8);
  EXPECT(cost.retires <= 1);
}
void push_steps(unsigned producers, long per) {
  wtc::rt::segment_queue<std::uint64_t, false, 64, 64, hooks, kOne> q;
  std::atomic<bool> go{false};
  std::atomic<unsigned> left{producers};
  std::atomic<std::uint64_t> fwd{0}, back{0}, back_total{0};
  std::vector<std::thread> ts;
  for (unsigned k = 0; k < producers; ++k)
    ts.emplace_back([&, k] {
      t_jitter = 29u + k;
      std::mt19937 r(k + 1);
      while (!go.load()) {
      }
      std::uint64_t worst_f = 0, worst_b = 0, sum_b = 0;
      for (long i = 0; i < per;) {
        std::uint64_t run[8];
        long m = r() % 4 == 0 ? 1 + static_cast<long>(r() % 8) : 1;
        if (i + m > per) m = per - i;
        for (long j = 0; j < m; ++j) run[j] = std::uint64_t{k} << 40 | static_cast<std::uint64_t>(i + j);
        t_fwd = t_back = 0;
        q.push_run(run, static_cast<std::size_t>(m));
        if (t_fwd > worst_f) worst_f = t_fwd;
        if (t_back > worst_b) worst_b = t_back;
        sum_b += t_back;
        i += m;
      }
      std::uint64_t w = fwd.load();
      while (worst_f > w && !fwd.compare_exchange_weak(w, worst_f)) {
      }
      w = back.load();
      while (worst_b > w && !back.compare_exchange_weak(w, worst_b)) {
      }
      back_total.fetch_add(sum_b);
      left.fetch_sub(1);
    });
  go.store(true);
  std::vector<long> next(producers, 0);
  std::uint64_t v = 0, got = 0;
  while (got < producers * static_cast<std::uint64_t>(per)) {
    if (q.try_pop(v)) {
      const unsigned k = static_cast<unsigned>(v >> 40);
      EXPECT(static_cast<long>(v & ((std::uint64_t{1} << 40) - 1)) == next[k]);
      ++next[k];
      ++got;
    }
  }
  for (auto& t : ts) t.join();
  EXPECT(!q.try_pop(v));
  std::printf("push_steps P=%2u: %llu elements; per push at most %llu segments forward, %llu back "
              "(%llu back in all)\n",
              producers, (unsigned long long)got, (unsigned long long)fwd.load(),
              (unsigned long long)back.load(), (unsigned long long)back_total.load());
  EXPECT(fwd.load() <= 1 + producers * 8 / 64 + 1);
}
}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "late_fill";
  if (!std::strcmp(mode, "late_fill")) late_fill();
  else if (!std::strcmp(mode, "two_holes")) two_holes();
  else if (!std::strcmp(mode, "holes")) {
    const unsigned p = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 4;
    const std::uint64_t n = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 400000;
    holes(p, n);
  } else if (!std::strcmp(mode, "push_steps")) {
    const unsigned p = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 4;
    const long n = argc > 3 ? std::atol(argv[3]) : 200000;
    push_steps(p, n);
  } else {
    std::fprintf(stderr, "mpsc_bound_test: unknown mode %s\n", mode);
    return 2;
  }
  if (failures) {
    std::fprintf(stderr, "mpsc_bound_test %s: %d failure(s)\n", mode, failures);
    return 1;
  }
  std::printf("mpsc_bound_test %s: ok\n", mode);
  return 0;
}
