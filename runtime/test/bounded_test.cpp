// Tests of the bounded queue (runtime/bounded_ring.hpp) through the shim's
// wtc::bounded (shim/bounded.hpp), on every topology and wait policy.
//
//   bounded_test model [N]     random operations, one thread, against a
//                              sequential bounded queue: every answer and
//                              every size
//   bounded_test exact         a pop held between taking its ticket and
//                              moving its element out: pushes still see
//                              the room its ticket made, and a size that
//                              counts the element as gone
//   bounded_test stress [N]    producers that wait while the queue is full,
//                              consumers that wait while it is empty, at
//                              capacities 1 to 3, with random pauses
//   bounded_test pingfull [N]  one producer kept against a full queue of
//                              one, a consumer taking one element at a time
//                              after a random delay: the lost wake-up of
//                              the "not full" side
//   bounded_test close         close() with producers asleep on a full
//                              queue and consumers asleep on an empty one
//   bounded_test discard [N]   try_push from many producers: what was
//                              refused is what never arrives
//   bounded_test overrun [N]   push_overrun from many producers against a
//                              consumer: pushes = pops + overruns + left
//   bounded_test twoproducers  two producers on a queue built for one, which
//                              the contract check must catch
//   bounded_push_steps split   (built with WTC_TEST_PUSH_STEPS) a push held
//                              between reading tail and reading head while
//                              another producer pushes three and a consumer
//                              pops them: the queue is empty throughout, and
//                              the push must not be refused as full
//
// Every element must arrive at most once (exactly once where nothing is
// dropped), in increasing order per producer for each consumer, and no
// size read may exceed the capacity. A lost wake-up leaves a thread asleep
// with the condition it waits for already true; the modes that can see one
// wait with a deadline and, past it, say "lost wake-up" and exit. With
// WTC_TEST_DROP_WAKEUPS the waiters never wake a sleeper on a publish, and
// pingfull must report the lost wake-up: the test that proves it can fail.

#include "../../shim/bounded.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
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
using wtc::rt::wait_policy;

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

bool manyP(topology t) { return t == topology::mpsc || t == topology::generic; }
bool manyC(topology t) { return t == topology::spmc || t == topology::generic; }

constexpr topology kTags[] = {topology::spsc, topology::mpsc, topology::spmc, topology::generic};
constexpr wait_policy kPolicies[] = {wait_policy::spin, wait_policy::park, wait_policy::spin_then_park};

// An element: the producer and its sequence number, and a payload that is
// not trivially movable, so that a move that went wrong shows.
struct item {
  std::uint32_t producer = 0;
  std::uint64_t seq = 0;
  std::string pad;
  item() = default;
  item(std::uint32_t p, std::uint64_t s) : producer(p), seq(s), pad(24, char('a' + s % 26)) {}
  item(item&&) noexcept = default;
  item& operator=(item&&) noexcept = default;
  bool intact() const { return pad.size() == 24 && pad[0] == char('a' + seq % 26); }
};

void lost_wakeup(const char* what) {
  std::fprintf(stderr, "lost wake-up: %s\n", what);
  std::fflush(stderr);
  std::_Exit(3);
}

// ---------------------------------------------------------------------------
// model: one thread, random operations, against std::deque with the
// original ring's rules.

void model(long n) {
  std::mt19937_64 rng(42);
  for (bool overrun : {false, true})
    for (topology tag : kTags)
      for (std::size_t cap = 0; cap <= 6; ++cap) {
        wtc::bounded<item> q(tag, cap, overrun, wait_policy::spin);
        std::deque<std::uint64_t> m;
        std::size_t overruns = 0;
        std::uint64_t next = 0;
        for (long i = 0; i < n; ++i) {
          const unsigned op = rng() % (overrun ? 5 : 4);
          if (op == 0) {
            const bool ok = q.try_push(item(0, next));
            EXPECT(ok == (m.size() < cap));
            if (ok) m.push_back(next);
            ++next;
          } else if (op == 1 && m.size() < cap) {
            EXPECT(q.push_wait(item(0, next)));
            m.push_back(next++);
          } else if (op == 4) {
            const bool dropped = q.push_overrun(item(0, next));
            const bool full = m.size() >= cap;
            EXPECT(dropped == full);
            if (cap == 0) {
              ++overruns;
            } else {
              if (full) {
                m.pop_front();
                ++overruns;
              }
              m.push_back(next);
            }
            ++next;
          } else {
            item out;
            const bool ok = q.try_pop(out);
            EXPECT(ok == !m.empty());
            if (ok) {
              EXPECT(out.seq == m.front());
              EXPECT(out.intact());
              m.pop_front();
            }
          }
          EXPECT(q.size() == m.size());
          EXPECT(q.overrun_counter() == overruns);
        }
      }
  std::printf("model: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// exact: an element whose move-assignment holds the consumer between its
// ticket and the cell.

struct gate {
  std::atomic<bool> at{false}, go{false};
  void stop_here() {
    at.store(true);
    while (!go.load()) std::this_thread::yield();
  }
  void wait_arrival() const {
    while (!at.load()) std::this_thread::yield();
  }
  void release() { go.store(true); }
};

gate g_hold;
std::atomic<int> g_hold_id{-1};

struct held {
  int id = 0;
  held() = default;
  explicit held(int i) : id(i) {}
  held(held&& o) noexcept : id(o.id) {}
  held& operator=(held&& o) noexcept {
    if (o.id == g_hold_id.load()) g_hold.stop_here();
    id = o.id;
    return *this;
  }
};

void exact() {
  for (bool overrun : {false, true}) {
    // Two consumers: the general topology. Capacity 2, so the ring is two
    // cells and the push of ticket 2 needs the cell the held pop still
    // reads.
    wtc::bounded<held> q(topology::generic, 2, overrun, wait_policy::spin);
    g_hold.at = false;
    g_hold.go = false;
    g_hold_id = 1;
    EXPECT(q.try_push(held(1)));
    EXPECT(q.try_push(held(2)));
    EXPECT(!q.try_push(held(3)));  // full
    std::thread c1([&] {
      held out;
      EXPECT(q.try_pop(out) && out.id == 1);
    });
    g_hold.wait_arrival();  // c1 has ticket 0 and is moving element 1 out
    // The held pop has taken effect: one element left, room for one.
    EXPECT(q.size() == 1);
    held out;
    EXPECT(q.try_pop(out) && out.id == 2);
    EXPECT(q.size() == 0);
    // Room for two, although the cell of ticket 0 is still being read: a
    // queue that decided fullness by its cells would refuse the first of
    // these. The first push takes ticket 2, whose cell is the held one, and
    // waits in it; the second takes ticket 3 and returns at once.
    std::atomic<bool> first_done{false};
    std::atomic<bool> first_ok{false};
    std::thread p1([&] {
      first_ok = q.try_push(held(3));
      first_done = true;
    });
    while (q.size() != 1) std::this_thread::yield();  // p1 has its ticket
    EXPECT(q.try_push(held(4)));
    EXPECT(q.size() == 2);
    EXPECT(!q.try_push(held(5)));  // and now it is full
    EXPECT(!first_done.load());    // p1 waits for the cell, not for room
    g_hold.release();
    c1.join();
    p1.join();
    EXPECT(first_ok.load());
    EXPECT(q.try_pop(out) && out.id == 3);
    EXPECT(q.try_pop(out) && out.id == 4);
    EXPECT(!q.try_pop(out));
    g_hold_id = -1;
  }
  std::printf("exact: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// stress: waiting on both sides.

struct seen_check {
  // Per consumer, the last sequence number seen from each producer.
  std::vector<std::vector<std::int64_t>> last;
  std::vector<std::atomic<std::uint8_t>> got;
  seen_check(unsigned consumers, unsigned producers, std::size_t total)
      : last(consumers, std::vector<std::int64_t>(producers, -1)), got(total) {}
  void see(unsigned c, const item& e, std::uint64_t per) {
    EXPECT(e.intact());
    EXPECT(static_cast<std::int64_t>(e.seq) > last[c][e.producer]);
    last[c][e.producer] = static_cast<std::int64_t>(e.seq);
    const std::uint8_t before = got[e.producer * per + e.seq].fetch_add(1);
    EXPECT(before == 0);
  }
};

void wait_all(std::vector<std::thread>& ts, std::atomic<unsigned>& done, unsigned n, const char* what) {
  const auto until = clk::now() + kDeadline;
  while (done.load() < n) {
    if (clk::now() > until) lost_wakeup(what);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (auto& t : ts) t.join();
  ts.clear();
}

void stress(long n) {
  for (topology tag : kTags)
    for (wait_policy pol : kPolicies)
      for (std::size_t cap : {1, 2, 3}) {
        const unsigned P = manyP(tag) ? 3 : 1, C = manyC(tag) ? 3 : 1;
        const std::uint64_t per = static_cast<std::uint64_t>(n);
        wtc::bounded<item> q(tag, cap, false, pol);
        seen_check chk(C, P, P * per);
        std::atomic<unsigned> done{0};
        std::atomic<std::uint64_t> taken{0};
        std::vector<std::thread> ts;
        for (unsigned p = 0; p < P; ++p)
          ts.emplace_back([&, p] {
            std::mt19937 rng(p + 1);
            for (std::uint64_t i = 0; i < per; ++i) {
              EXPECT(q.push_wait(item(p, i)));
              if (rng() % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
            }
            ++done;
          });
        for (unsigned c = 0; c < C; ++c)
          ts.emplace_back([&, c] {
            std::mt19937 rng(100 + c);
            item e;
            while (q.pop_wait(e)) {
              chk.see(c, e, per);
              EXPECT(q.size() <= cap);
              ++taken;
              if (rng() % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
            }
            ++done;
          });
        // Every element taken, then close to let the consumers go.
        const auto until = clk::now() + kDeadline;
        while (taken.load() < P * per) {
          if (clk::now() > until) lost_wakeup("stress: an element waits with a consumer asleep, or a "
                                              "producer sleeps with room");
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        q.close();
        wait_all(ts, done, P + C, "stress: a consumer sleeps through close()");
        for (std::size_t i = 0; i < P * per; ++i) EXPECT(chk.got[i].load() == 1);
        if (g_failures) {
          std::printf("stress %s %s cap %zu: FAILED\n", name(tag), name(pol), cap);
          return;
        }
      }
  std::printf("stress: ok\n");
}

// ---------------------------------------------------------------------------
// pingfull: the producer's side of the lost wake-up. Capacity 1; the
// producer pushes as fast as it can and so is almost always against a full
// queue, asleep or about to be; the consumer takes one element, then
// pauses a random time, which lands its pop in every phase of the
// producer going to sleep.

void pingfull(long n) {
  for (topology tag : kTags)
    for (wait_policy pol : {wait_policy::park, wait_policy::spin_then_park}) {
      wtc::bounded<item> q(tag, 1, false, pol);
      std::atomic<std::uint64_t> pushed{0};
      std::thread prod([&] {
        for (long i = 0; i < n; ++i) {
          q.push_wait(item(0, static_cast<std::uint64_t>(i)));
          pushed.store(static_cast<std::uint64_t>(i) + 1);
        }
      });
      std::mt19937 rng(7);
      item e;
      for (long i = 0; i < n; ++i) {
        const auto until = clk::now() + kDeadline;
        while (!q.try_pop(e)) {
          if (clk::now() > until) lost_wakeup("pingfull: the queue is empty and its producer asleep");
          std::this_thread::yield();
        }
        EXPECT(e.seq == static_cast<std::uint64_t>(i));
        // The producer must get past the room this pop made: the element
        // after it arrives, or the producer is done.
        const auto until2 = clk::now() + kDeadline;
        while (pushed.load() < static_cast<std::uint64_t>(i) + 2 && pushed.load() < static_cast<std::uint64_t>(n)) {
          if (clk::now() > until2)
            lost_wakeup("pingfull: a pop made room and the producer slept on (lost wake-up: a publish "
                        "woke nobody)");
          std::this_thread::yield();
        }
        const unsigned d = rng() % 200;
        if (d < 100) {
          for (unsigned k = 0; k < d * 10; ++k) std::atomic_signal_fence(std::memory_order_seq_cst);
        } else {
          std::this_thread::sleep_for(std::chrono::microseconds(d - 100));
        }
      }
      prod.join();
    }
  std::printf("pingfull: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// close: sleepers on both sides.

void close_test() {
  for (topology tag : {topology::generic, topology::mpsc, topology::spmc})
    for (wait_policy pol : kPolicies) {
      // Producers asleep on a full queue. The test's own thread pushes
      // and pops too, so only where there may be several producers.
      if (manyP(tag)) {
        wtc::bounded<item> q(tag, 1, false, pol);
        EXPECT(q.try_push(item(0, 0)));
        const unsigned P = 4;
        std::atomic<unsigned> done{0}, refused{0};
        std::vector<std::thread> ts;
        for (unsigned p = 0; p < P; ++p)
          ts.emplace_back([&, p] {
            item v(p, 1);
            if (!q.push_wait(std::move(v))) {
              ++refused;
              EXPECT(v.intact());  // a refused push leaves its argument alone
            }
            ++done;
          });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT(done.load() == 0);
        q.close();
        wait_all(ts, done, P, "close: a producer sleeps through close()");
        EXPECT(refused.load() == P);
        item e;
        EXPECT(q.try_pop(e) && e.seq == 0);  // what was there stays
        EXPECT(q.try_push(item(0, 2)));      // and try_push is unchanged
      }
      // Consumers asleep on an empty queue.
      {
        wtc::bounded<item> q(tag, 4, false, pol);
        const unsigned C = manyC(tag) ? 4 : 1;
        std::atomic<unsigned> done{0}, got{0};
        std::vector<std::thread> ts;
        for (unsigned c = 0; c < C; ++c)
          ts.emplace_back([&] {
            item e;
            while (q.pop_wait(e)) ++got;
            ++done;
          });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT(q.push_wait(item(0, 0)));
        EXPECT(q.push_wait(item(0, 1)));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        q.close();
        wait_all(ts, done, C, "close: a consumer sleeps through close()");
        EXPECT(got.load() == 2);
      }
    }
  // The timed pop gives up with the queue empty, and returns an element
  // pushed while it waits.
  {
    wtc::bounded<item> q(topology::spsc, 2, false, wait_policy::spin_then_park);
    item e;
    const auto t0 = clk::now();
    EXPECT(!q.pop_wait_for(e, std::chrono::milliseconds(20)));
    EXPECT(clk::now() - t0 >= std::chrono::milliseconds(20));
    std::thread p([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      q.push_wait(item(0, 5));
    });
    EXPECT(q.pop_wait_for(e, std::chrono::seconds(10)) && e.seq == 5);
    p.join();
  }
  std::printf("close: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// discard and overrun under contention.

void discard(long n) {
  for (topology tag : {topology::mpsc, topology::generic}) {
    const unsigned P = 4, C = manyC(tag) ? 2 : 1;
    const std::uint64_t per = static_cast<std::uint64_t>(n);
    wtc::bounded<item> q(tag, 8, false, wait_policy::spin_then_park);
    seen_check chk(C, P, P * per);
    std::atomic<std::uint64_t> accepted{0}, refused{0}, taken{0};
    std::atomic<unsigned> done{0};
    std::vector<std::thread> ts;
    for (unsigned p = 0; p < P; ++p)
      ts.emplace_back([&, p] {
        for (std::uint64_t i = 0; i < per; ++i) (q.try_push(item(p, i)) ? accepted : refused)++;
        ++done;
      });
    for (unsigned c = 0; c < C; ++c)
      ts.emplace_back([&, c] {
        item e;
        while (q.pop_wait(e)) {
          chk.see(c, e, per);
          EXPECT(q.size() <= 8);
          ++taken;
        }
        ++done;
      });
    while (done.load() < P) std::this_thread::yield();
    const auto until = clk::now() + kDeadline;
    while (taken.load() < accepted.load()) {
      if (clk::now() > until) lost_wakeup("discard: an accepted element waits with its consumer asleep");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    q.close();
    wait_all(ts, done, P + C, "discard: a consumer sleeps through close()");
    EXPECT(accepted.load() + refused.load() == P * per);
    EXPECT(taken.load() == accepted.load());
  }
  std::printf("discard: %s\n", g_failures ? "FAILED" : "ok");
}

void overrun(long n) {
  for (std::size_t cap : {1, 3, 8}) {
    const unsigned P = 4;
    const std::uint64_t per = static_cast<std::uint64_t>(n);
    // The tag says many consumers: every overrunning producer is one.
    wtc::bounded<item> q(topology::generic, cap, true, wait_policy::spin_then_park);
    seen_check chk(1, P, P * per);
    std::atomic<std::uint64_t> taken{0};
    std::atomic<unsigned> done{0};
    std::vector<std::thread> ts;
    for (unsigned p = 0; p < P; ++p)
      ts.emplace_back([&, p] {
        for (std::uint64_t i = 0; i < per; ++i) {
          q.push_overrun(item(p, i));
          EXPECT(q.size() <= cap);
        }
        ++done;
      });
    std::thread cons([&] {
      item e;
      while (q.pop_wait(e)) {
        chk.see(0, e, per);
        EXPECT(q.size() <= cap);
        ++taken;
      }
    });
    for (auto& t : ts) t.join();
    q.close();
    cons.join();
    // Every push either arrived or was overrun, once.
    std::uint64_t arrived = 0;
    for (std::size_t i = 0; i < P * per; ++i) arrived += chk.got[i].load();
    EXPECT(arrived == taken.load());
    EXPECT(taken.load() + q.overrun_counter() == P * per);
    EXPECT(q.size() == 0);
  }
  std::printf("overrun: %s\n", g_failures ? "FAILED" : "ok");
}

// twoproducers: a queue built for one producer, pushed by two. Only the
// contract check (WTC_CHECK) catches it, and must.
void two_producers() {
  wtc::bounded<item> q(topology::spsc, 1024, false, wait_policy::spin);
  std::thread a([&] {
    for (int i = 0; i < 100; ++i) q.try_push(item(0, 0));
  });
  std::thread b([&] {
    for (int i = 0; i < 100; ++i) q.try_push(item(1, 0));
  });
  a.join();
  b.join();
}

#ifdef WTC_TEST_PUSH_STEPS
// split: the split layout (no overrun) with several producers. A push
// reads tail, is held here, and then reads head; meanwhile another
// producer pushes three elements and the consumer pops them, so head
// passes the tail the held push read. The queue never holds more than
// three of its four, and the held push must append its element.
thread_local bool t_hold = false;
std::atomic<bool> g_held{false}, g_release{false};

void split_push_steps() {
  for (topology tag : {topology::mpsc, topology::generic}) {
    g_held = false;
    g_release = false;
    wtc::bounded<item> q(tag, 4, false, wait_policy::spin);
    bool pushed = false;
    std::thread held([&] {
      t_hold = true;
      pushed = q.try_push(item(0, 42));
    });
    while (!g_held.load()) std::this_thread::yield();
    std::thread other([&] {
      for (int i = 0; i < 3; ++i) q.try_push(item(1, i));
    });
    other.join();
    item out;
    for (int i = 0; i < 3; ++i) EXPECT(q.try_pop(out));
    EXPECT(q.size() == 0);  // empty while the held push runs
    g_release = true;
    held.join();
    if (!pushed)
      std::fprintf(stderr, "split (%s): a push held while head passed the tail it read was refused as full "
                           "on an empty queue\n", name(tag));
    EXPECT(pushed);
    EXPECT(q.try_pop(out) && out.seq == 42);  // the held push's element arrives
  }
  std::printf("split: %s\n", g_failures ? "FAILED" : "ok");
}
#endif

}  // namespace

#ifdef WTC_TEST_PUSH_STEPS
extern "C" void wtc_test_push_step() {
  if (!t_hold) return;
  t_hold = false;
  g_held = true;
  while (!g_release.load()) std::this_thread::yield();
}
#endif

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: bounded_test model|exact|stress|pingfull|close|discard|overrun [N]\n");
    return 2;
  }
  const std::string mode = argv[1];
  const long n = argc > 2 ? std::atol(argv[2]) : 0;
  if (mode == "model") model(n ? n : 20000);
  else if (mode == "exact") exact();
  else if (mode == "stress") stress(n ? n : 20000);
  else if (mode == "pingfull") pingfull(n ? n : 2000);
  else if (mode == "close") close_test();
  else if (mode == "discard") discard(n ? n : 100000);
  else if (mode == "overrun") overrun(n ? n : 100000);
  else if (mode == "twoproducers") two_producers();
#ifdef WTC_TEST_PUSH_STEPS
  else if (mode == "split") split_push_steps();
#endif
  else {
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
  }
  return g_failures ? 1 : 0;
}
