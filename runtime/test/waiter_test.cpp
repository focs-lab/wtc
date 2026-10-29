// Tests of the waiting pop (runtime/waiter.hpp) through the shim's
// wtc::queue, on every topology and every wait policy.
//
//   waiter_test interleave       the lost wake-up's window, forced
//   waiter_test pingpong [N]     one element at a time, a consumer asleep
//   waiter_test stress [N]       many elements, producers pausing at random
//   waiter_test close            close() with consumers asleep, and drain
//   waiter_test timed            pop_wait_for's limit
//
// A lost wake-up leaves a consumer asleep with an element queued and no
// push to come. Each mode that can see one waits for its consumers with a
// deadline and, past it, says "lost wake-up" and exits: interleave holds
// the consumer exactly between its last look and its sleep while the
// producer publishes; pingpong pushes one element (or one per consumer)
// and waits for it to be taken before pushing the next, after a random
// delay that lands the push in every phase of a consumer going to sleep;
// stress waits, before it closes the queue, for every element to be taken.
// With WTC_TEST_DROP_WAKEUPS the waiter never wakes a sleeper on a publish,
// and interleave must report the lost wake-up: the test that proves the
// test can fail.
//
// Every element must arrive exactly once, and each consumer must see each
// producer's elements in increasing order (FIFO per producer). The
// topologies are kept: one producer for spsc, spsc_inline and spmc, one
// consumer for spsc, spsc_inline and mpsc, so that WTC_CHECK builds pass.

#include "../shim/queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(__SANITIZE_THREAD__)
#define WTC_TEST_SLOW 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WTC_TEST_SLOW 1
#endif
#endif

namespace {

using clk = std::chrono::steady_clock;
using wtc::topology;
using wtc::wait_policy;
using elem = std::uint64_t;

#ifdef WTC_TEST_SLOW
constexpr auto kDeadline = std::chrono::seconds(60);
#else
constexpr auto kDeadline = std::chrono::seconds(20);
#endif

std::atomic<int> g_failures{0};
#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

const char* name(topology t) {
  switch (t) {
  case topology::spsc: return "spsc";
  case topology::spsc_inline: return "spsc_inline";
  case topology::mpsc: return "mpsc";
  case topology::spmc: return "spmc";
  default: return "generic";
  }
}
const char* name(wait_policy p) {
  switch (p) {
  case wait_policy::spin: return "spin";
  case wait_policy::park: return "park";
  default: return "spin_then_park";
  }
}

constexpr topology kTopologies[] = {topology::spsc, topology::spsc_inline, topology::mpsc,
                                    topology::spmc, topology::generic};
constexpr wait_policy kPolicies[] = {wait_policy::spin, wait_policy::park,
                                     wait_policy::spin_then_park};

unsigned producers_of(topology t) {
  return t == topology::mpsc || t == topology::generic ? 3 : 1;
}
unsigned consumers_of(topology t) {
  return t == topology::spmc || t == topology::generic ? 3 : 1;
}

[[noreturn]] void lost(const char* what, topology t, wait_policy p) {
  std::fprintf(stderr, "lost wake-up: %s (%s, %s)\n", what, name(t), name(p));
  std::fflush(stderr);
  std::_Exit(2);
}

// Waits for c to reach n, or reports a lost wake-up.
void await(const std::atomic<std::uint64_t>& c, std::uint64_t n, const char* what, topology t,
           wait_policy p) {
  const auto until = clk::now() + kDeadline;
  while (c.load(std::memory_order_acquire) < n) {
    if (clk::now() > until) lost(what, t, p);
    std::this_thread::yield();
  }
}

elem make(unsigned producer, std::uint64_t seq) { return (elem{producer} << 40) | seq; }
unsigned producer_of(elem e) { return static_cast<unsigned>(e >> 40); }
std::uint64_t seq_of(elem e) { return e & ((elem{1} << 40) - 1); }

// What the consumers saw: each element once, each producer's in order.
struct ledger {
  explicit ledger(unsigned producers, std::uint64_t per) : per_(per), seen(producers * per) {}
  std::uint64_t per_;
  std::vector<std::atomic<unsigned char>> seen;
  std::atomic<std::uint64_t> taken{0};
  std::atomic<bool> ok{true};

  // A consumer's record of the last sequence number per producer.
  struct mine {
    std::uint64_t last[8] = {};
    bool any[8] = {};
  };
  void take(elem e, mine& m) {
    const unsigned p = producer_of(e);
    const std::uint64_t s = seq_of(e);
    if (p >= 8 || s >= per_) {
      bad("an element no producer pushed");
      return;
    }
    if (m.any[p] && s <= m.last[p]) bad("a producer's elements out of order");
    m.any[p] = true;
    m.last[p] = s;
    if (seen[p * per_ + s].exchange(1, std::memory_order_relaxed)) bad("an element popped twice");
    taken.fetch_add(1, std::memory_order_release);
  }
  void bad(const char* why) {
    if (ok.exchange(false)) std::fprintf(stderr, "%s\n", why);
  }
};

// The lost wake-up's window, forced: the consumer has announced itself and
// made its last look, which found nothing, and is about to sleep; the
// producer now publishes and notifies. The waiter must not sleep through
// that. With a park policy the pop's looks are, in order: the first, then
// the one after announcing. The second is held until the producer has
// published and notified, and then answers "nothing", as a look made just
// before the publish would.
void interleave() {
  for (int round = 0; round < 2; ++round) {
    wtc::rt::waiter w(wait_policy::park);
    std::atomic<bool> has{false};
    std::atomic<int> looks{0};
    std::atomic<bool> at{false}, go{false};
    std::atomic<int> result{-1};
    std::thread consumer([&] {
      const bool got = w.wait([&] {
        const int k = ++looks;
        if (k == 2) {
          at.store(true, std::memory_order_release);
          while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
          return false;
        }
        return has.exchange(false);
      });
      result.store(got ? 1 : 0, std::memory_order_release);
    });
    while (!at.load(std::memory_order_acquire)) std::this_thread::yield();
    // Announced before its last look: the producer must see it.
    EXPECT(w.waiting() == 1);
    if (round == 0) {
      has.store(true, std::memory_order_release);
      w.notify(1);
    } else {
      w.close();
    }
    go.store(true, std::memory_order_release);
    const auto until = clk::now() + std::chrono::seconds(3);
    while (result.load(std::memory_order_acquire) < 0) {
      if (clk::now() > until) {
        std::fprintf(stderr, "lost wake-up: a %s between the last look and the sleep\n",
                     round == 0 ? "publish" : "close");
        std::fflush(stderr);
        std::_Exit(2);
      }
      std::this_thread::yield();
    }
    consumer.join();
    EXPECT(result.load() == (round == 0 ? 1 : 0));
    EXPECT(w.waiting() == 0);
  }
}

// One element (or one per consumer) at a time, each pushed after a random
// delay of up to about 30 µs, then waited for before the next.
void pingpong(std::uint64_t rounds) {
  for (topology t : kTopologies) {
    for (wait_policy p : kPolicies) {
      wtc::queue<elem> q(t, p);
      const unsigned nc = consumers_of(t);
      ledger led(1, rounds * nc);
      std::atomic<std::uint64_t> returned{0};
      std::vector<std::thread> cs;
      for (unsigned c = 0; c < nc; ++c)
        cs.emplace_back([&] {
          ledger::mine m;
          elem e;
          while (q.pop_wait(e)) led.take(e, m);
          returned.fetch_add(1, std::memory_order_release);
        });
      std::mt19937_64 rng(42);
      std::uint64_t seq = 0;
      for (std::uint64_t r = 0; r < rounds; ++r) {
        const unsigned burst = 1 + static_cast<unsigned>(rng() % nc);
        const auto delay = std::chrono::nanoseconds(rng() % 30000);
        const auto until = clk::now() + delay;
        while (clk::now() < until) {
        }
        for (unsigned k = 0; k < burst; ++k) q.push(make(0, seq++));
        await(led.taken, seq, "a consumer slept through a push", t, p);
      }
      q.close();
      await(returned, nc, "a consumer asleep through close()", t, p);
      for (auto& c : cs) c.join();
      EXPECT(led.ok.load());
      EXPECT(led.taken.load() == seq);
      if (!led.ok.load()) std::fprintf(stderr, "  in pingpong (%s, %s)\n", name(t), name(p));
    }
  }
}

// Producers push n elements each, by push and in runs published together,
// pausing at random: mostly not at all, now and then for some
// microseconds of spinning, and now and then asleep for up to half a
// millisecond, so that the consumers park and wake all the time.
void stress(std::uint64_t n) {
  for (topology t : kTopologies) {
    for (wait_policy p : kPolicies) {
      wtc::queue<elem> q(t, p);
      const unsigned np = producers_of(t), nc = consumers_of(t);
      ledger led(np, n);
      std::atomic<std::uint64_t> returned{0};
      std::vector<std::thread> cs, ps;
      for (unsigned c = 0; c < nc; ++c)
        cs.emplace_back([&] {
          ledger::mine m;
          elem e;
          while (q.pop_wait(e)) led.take(e, m);
          returned.fetch_add(1, std::memory_order_release);
        });
      for (unsigned pr = 0; pr < np; ++pr)
        ps.emplace_back([&, pr] {
          std::mt19937_64 rng(pr * 7919 + 1);
          unsigned run = 1;
          for (std::uint64_t i = 0; i < n;) {
            const std::uint64_t dice = rng() % 1024;
            if (dice < 4) {
              std::this_thread::sleep_for(std::chrono::microseconds(rng() % 500));
            } else if (dice < 64) {
              const auto until = clk::now() + std::chrono::nanoseconds(rng() % 5000);
              while (clk::now() < until) {
              }
            }
            if (run == 1) {
              q.push(make(pr, i++));
            } else {
              wtc::queue<elem>::batch b;
              q.batch_begin(b);
              for (unsigned k = 0; k < run && i < n; ++k) q.push_deferred(b, make(pr, i++));
              q.batch_end(b);
            }
            run = run % 97 + 1;
          }
        });
      for (auto& th : ps) th.join();
      // Every element taken before the close: a consumer asleep with
      // elements queued and nothing more to come would never take them.
      await(led.taken, np * n, "elements queued and every consumer asleep", t, p);
      q.close();
      await(returned, nc, "a consumer asleep through close()", t, p);
      for (auto& th : cs) th.join();
      EXPECT(led.ok.load());
      EXPECT(led.taken.load() == np * n);
      if (!led.ok.load()) std::fprintf(stderr, "  in stress (%s, %s)\n", name(t), name(p));
    }
  }
}

// close() wakes every consumer asleep on an empty queue, and the elements
// pushed before it are still all taken. Afterwards, on the closed and empty
// queue, a waiting pop returns at once, try_pop is as it was, and an
// element pushed later is still taken. The main thread is the producer and
// the first consumer thread does the checks after the close, so that every
// topology keeps its sides.
void close_wakes() {
  for (topology t : kTopologies) {
    for (wait_policy p : kPolicies) {
      for (std::uint64_t before : {std::uint64_t{0}, std::uint64_t{1000}}) {
        wtc::queue<elem> q(t, p);
        const unsigned nc = consumers_of(t);
        ledger led(1, before ? before : 1);
        std::atomic<std::uint64_t> returned{0};
        std::atomic<bool> pushed_late{false};
        std::vector<std::thread> cs;
        for (unsigned c = 0; c < nc; ++c)
          cs.emplace_back([&, c] {
            ledger::mine m;
            elem e = 0;
            while (q.pop_wait(e)) led.take(e, m);
            EXPECT(q.closed());
            if (c == 0) {
              EXPECT(!q.pop_wait(e));
              EXPECT(!q.pop_wait_for(e, std::chrono::seconds(10)));
              EXPECT(!q.try_pop(e));
            }
            returned.fetch_add(1, std::memory_order_release);
            if (c != 0) return;
            while (!pushed_late.load(std::memory_order_acquire)) std::this_thread::yield();
            EXPECT(q.pop_wait(e) && e == make(0, 7));
          });
        // Long enough for every consumer to be asleep, past any spin.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT(returned.load() == 0);
        for (std::uint64_t i = 0; i < before; ++i) q.push(make(0, i));
        q.close();
        await(returned, nc, "a consumer asleep through close()", t, p);
        q.push(make(0, 7));
        pushed_late.store(true, std::memory_order_release);
        for (auto& c : cs) c.join();
        EXPECT(led.ok.load());
        EXPECT(led.taken.load() == before);
      }
    }
  }
}

// pop_wait_for: gives up after its limit on an empty queue, not much
// sooner and not very much later; returns an element pushed within it;
// with a limit of zero looks once.
void timed() {
  for (topology t : kTopologies) {
    for (wait_policy p : kPolicies) {
      wtc::queue<elem> q(t, p);
      elem e = 0;
      EXPECT(!q.pop_wait_for(e, std::chrono::nanoseconds(0)));
      EXPECT(!q.pop_wait_for(e, std::chrono::nanoseconds(-5)));
      auto t0 = clk::now();
      EXPECT(!q.pop_wait_for(e, std::chrono::milliseconds(5)));
      auto took = clk::now() - t0;
      EXPECT(took >= std::chrono::milliseconds(5));
      EXPECT(took < std::chrono::seconds(2));
      // A limit much longer than any spin budget, the element pushed in it.
      // One producer thread for both elements, so that every topology
      // keeps one.
      std::thread producer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        q.push(make(0, 1));
        q.push(make(0, 2));
      });
      t0 = clk::now();
      EXPECT(q.pop_wait_for(e, std::chrono::seconds(30)));
      took = clk::now() - t0;
      EXPECT(e == make(0, 1));
      EXPECT(took < std::chrono::seconds(10));
      producer.join();
      EXPECT(q.pop_wait_for(e, std::chrono::nanoseconds(0)) && e == make(0, 2));
      // A limit no clock can reach is no limit.
      std::thread closer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        q.close();
      });
      EXPECT(!q.pop_wait_for(e, std::chrono::nanoseconds::max()));
      closer.join();
      if (g_failures.load()) std::fprintf(stderr, "  in timed (%s, %s)\n", name(t), name(p));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  const std::uint64_t n = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
  if (mode == "interleave") interleave();
  else if (mode == "pingpong") pingpong(n ? n : 2000);
  else if (mode == "stress") stress(n ? n : 100000);
  else if (mode == "close") close_wakes();
  else if (mode == "timed") timed();
  else {
    std::fprintf(stderr, "usage: waiter_test interleave|pingpong|stress|close|timed [n]\n");
    return 2;
  }
  if (g_failures.load()) {
    std::fprintf(stderr, "%s: %d failure(s)\n", mode.c_str(), g_failures.load());
    return 1;
  }
  std::printf("%s: ok\n", mode.c_str());
  return 0;
}
