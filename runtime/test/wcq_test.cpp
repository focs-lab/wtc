// The wait-free ring (index_ring.hpp, wcq_ring) under threads that stop.
//
//   wcq_test frozen [N]   one thread frozen at a point of its own operation
//                         -- after taking a position, holding a cell,
//                         starting a slow-path round, visiting a pending
//                         request, or at its k-th step for k from 1 to 40
//                         -- while four producers and three consumers make
//                         N operations each on a queue of capacity 4: they
//                         all finish, every element arrives once, and the
//                         most steps any of their operations took is
//                         printed. The paper proves the steps finite and
//                         gives no closed form; none is asserted beyond a
//                         guard against a runaway loop.
//   wcq_test ring [R [T [K]]]  R rounds (1000) of T threads (6) each
//                         enqueuing an index and then dequeuing one and
//                         enqueuing it back K times (2000) on a ring of
//                         eight indices: every index once in the ring at
//                         the end. The test that found four faults in the
//                         slow path (index_ring.hpp says which), each a few
//                         times in a thousand rounds; the slow-path build
//                         runs ten thousand, which the lookup's fault needs
//                         (about one round in a thousand).
//   wcq_test cas2         the build's and the processor's CAS2 agree, and
//                         the ring refuses neither
//   wcq_test slots        the thread slots (index_ring.hpp): sixteen idle
//                         threads that hold hazard records from another
//                         queue do not take a ring's slots; two thousand
//                         threads, at most four alive at once, on a ring
//                         built for four all find slots; and four threads
//                         alive on a ring built for two run correctly,
//                         two of them as fallbacks, counted
//   wcq_test over         three threads alive on a ring built for two;
//                         with WTC_CHECK the third must be refused
//   wcq_test helped       a slow-path enqueue whose entry a helper produced,
//                         the helper stopped before it resets the
//                         threshold: once the enqueue has returned, a
//                         dequeue must find the element, so the enqueue
//                         must reset the threshold itself
//   wcq_test late         two threads whose last round of key destructors
//                         operates on a ring built for two, after the
//                         hazard key's destructor gave their records back:
//                         those operations take no slot, so a later thread
//                         finds a slot and does not fall back
//   wcq_test fork         a fork while eight threads hold a ring's slots,
//                         one of them inside a slow-path enqueue: in the
//                         child a new thread takes a slot of theirs, the
//                         stopped enqueue is finished, and nothing falls
//                         back
//
// Built twice: with the ring's defaults, and with no patience for enqueues,
// one try for dequeues and help on every operation (WTC_TEST_WCQ_SLOW), so
// that almost every operation runs the slow path and every one helps.
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "index_ring.hpp"
#include "lemma_util.hpp"
#include "segment_queue.hpp"

namespace {

enum point { kEnq, kDeq, kPushCell, kPopCell, kSlowRound, kHelping, kProduced, kStep, kPoints };
[[maybe_unused]] const char* const kPointNames[] = {"its position (enqueue)", "its position (dequeue)", "a cell (push)",
                                   "a cell (pop)", "a slow-path round", "a helping visit",
                                   "an entry produced", "step"};

thread_local int t_freeze = -1;       // the point this thread stops at, once
thread_local unsigned t_freeze_step = 0;
thread_local std::uint64_t t_steps = 0;
lemma::gate g_frozen;

// The helped test's own hold, apart from the frozen test's gate.
thread_local bool t_hold_produced = false;
std::atomic<bool> g_produced_seen{false}, g_produced_go{false};

void at(point p) {
  if (t_freeze == p) {
    t_freeze = -1;
    g_frozen.stop_here();
  }
}

struct hooks : wtc::rt::no_ring_hooks {
  static void enq_reserved() { at(kEnq); }
  static void deq_reserved() { at(kDeq); }
  static void push_has_cell() { at(kPushCell); }
  static void pop_has_cell() { at(kPopCell); }
  static void slow_round() { at(kSlowRound); }
  static void helping() { at(kHelping); }
  static void produced() {
    if (t_hold_produced) {
      t_hold_produced = false;
      g_produced_seen.store(true);
      while (!g_produced_go.load()) std::this_thread::yield();
      return;
    }
    at(kProduced);
  }
  static void step() {
    ++t_steps;
    if (t_freeze == kStep && t_steps == t_freeze_step) at(kStep);
  }
};

#ifdef WTC_TEST_WCQ_SLOW
template <class H>
using ring = wtc::rt::wcq_ring<H, wtc::rt::wide_words, wtc::rt::thread_slots, 0, 1, 1>;
[[maybe_unused]] constexpr const char* kBuild = "slow-path build";
#else
template <class H>
using ring = wtc::rt::wcq_ring<H>;
[[maybe_unused]] constexpr const char* kBuild = "default build";
#endif
using queue = wtc::rt::index_queue<std::uint64_t, hooks, ring>;
using ring_t = ring<wtc::rt::no_ring_hooks>;

#ifndef WTC_TEST_EXPECT_NO_CAS2
// Runs the workers and a thread that freezes at `where` among them;
// returns the most steps one of the workers' operations took that began
// while that thread was frozen, or ~0 if it never got there.
std::uint64_t frozen_run(point where, unsigned step, long per, bool frozen_pushes) {
  constexpr unsigned kProducers = 4, kConsumers = 3;
  // The workers and the frozen thread; the main thread drains after
  // they have all exited, in a slot one of them held.
  queue q(4, true, kProducers + kConsumers + 1);
  g_frozen.reset();
  std::atomic<bool> workers_done{false};
  std::atomic<std::uint64_t> frozen_pushed{0}, popped{0}, sum{0}, frozen_seen{0};
  std::thread frozen([&] {
    t_freeze = where;
    t_freeze_step = step;
    std::uint64_t v = 0, i = 0;
    while (t_freeze != -1 && !workers_done.load()) {
      t_steps = 0;
      if (frozen_pushes) {
        if (q.try_push((std::uint64_t{9} << 40) | ++i)) frozen_pushed.fetch_add(1);
      } else if (q.try_pop(v)) {
        if ((v >> 40) == 9) {
          frozen_seen.fetch_add(1);
        } else {
          popped.fetch_add(1);
          sum.fetch_add(v & ((std::uint64_t{1} << 40) - 1));
        }
      }
    }
  });
  std::atomic<std::uint64_t> worst{0};
  std::atomic<unsigned> producers_left{kProducers};
  std::vector<std::thread> ts;
  auto note = [&](std::uint64_t s, bool counted) {
    if (!counted) return;
    std::uint64_t w = worst.load();
    while (s > w && !worst.compare_exchange_weak(w, s)) {
    }
  };
  for (unsigned p = 0; p < kProducers; ++p)
    ts.emplace_back([&, p] {
      for (long i = 0; i < per; ++i) {
        const std::uint64_t v = (std::uint64_t{p} << 40) | static_cast<std::uint64_t>(i);
        for (;;) {
          const bool during = g_frozen.at.load();
          t_steps = 0;
          const bool ok = q.try_push(v);
          note(t_steps, during);
          if (ok) break;
        }
      }
      producers_left.fetch_sub(1);
    });
  for (unsigned c = 0; c < kConsumers; ++c)
    ts.emplace_back([&] {
      std::uint64_t v;
      for (;;) {
        const bool during = g_frozen.at.load();
        t_steps = 0;
        const bool got = q.try_pop(v);
        note(t_steps, during);
        if (got) {
          if ((v >> 40) == 9) {
            frozen_seen.fetch_add(1);
          } else {
            popped.fetch_add(1);
            sum.fetch_add(v & ((std::uint64_t{1} << 40) - 1));
          }
        } else if (producers_left.load() == 0) {
          // Every element pushed is taken, by a consumer or by a frozen
          // popper that holds it.
          break;
        }
      }
    });
  for (auto& t : ts) t.join();
  const bool was_frozen = g_frozen.at.load();
  workers_done = true;
  g_frozen.release();
  frozen.join();
  // Every producer's element once, and every element the frozen pusher
  // pushed, popped or still queued.
  std::uint64_t v;
  while (q.try_pop(v)) {
    if ((v >> 40) == 9) {
      frozen_seen.fetch_add(1);
    } else {
      popped.fetch_add(1);
      sum.fetch_add(v & ((std::uint64_t{1} << 40) - 1));
    }
  }
  if (popped.load() != kProducers * static_cast<std::uint64_t>(per) || frozen_seen.load() != frozen_pushed.load())
    std::fprintf(stderr, "frozen at %d/%u: size %zu after the drain (%s); popped %llu of %llu, the frozen pusher's %llu of %llu\n", where, step, q.size(),
                 frozen_pushes ? "pusher" : "popper", (unsigned long long)popped.load(),
                 (unsigned long long)(kProducers * per), (unsigned long long)frozen_seen.load(),
                 (unsigned long long)frozen_pushed.load());
  EXPECT(popped.load() == kProducers * static_cast<std::uint64_t>(per));
  EXPECT(sum.load() == kProducers * static_cast<std::uint64_t>(per) * (per - 1) / 2);
  EXPECT(frozen_seen.load() == frozen_pushed.load());
  EXPECT(q.free_ring().fallbacks() == 0 && q.queued_ring().fallbacks() == 0);
  return was_frozen ? worst.load() : ~std::uint64_t{0};
}

void frozen(long per) {
  std::uint64_t overall = 0;
  for (int w = 0; w < kStep; ++w)
    for (bool pushes : {true, false}) {
      const std::uint64_t s = frozen_run(static_cast<point>(w), 0, per, pushes);
      if (s == ~std::uint64_t{0}) {
        std::printf("frozen at %s (%s): not reached in this build\n", kPointNames[w], pushes ? "pusher" : "popper");
        continue;
      }
      std::printf("frozen at %-24s (%s): the others' worst operation %llu steps\n", kPointNames[w],
                  pushes ? "pusher" : "popper", (unsigned long long)s);
      if (s > overall) overall = s;
    }
  for (unsigned k = 1; k <= 40; ++k)
    for (bool pushes : {true, false}) {
      const std::uint64_t s = frozen_run(kStep, k, per / 4, pushes);
      if (s != ~std::uint64_t{0} && s > overall) overall = s;
    }
  std::printf("frozen (%s): worst operation of the others over every freeze: %llu steps\n", kBuild,
              (unsigned long long)overall);
  // A guard against a runaway loop, not a bound (see the top).
  EXPECT(overall < 1000000);
}

// The ring alone, as index_queue uses it, under churn.
void ring_churn(long rounds, unsigned kThreads, int ops) {
  long bad = 0;
  for (long round = 0; round < rounds; ++round) {
    ring_t r(3, 0, 16);
    std::atomic<unsigned> go{0};
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < kThreads; ++t)
      ts.emplace_back([&, t] {
        go.fetch_add(1);
        while (go.load() < kThreads) {
        }
        r.enqueue(t);
        for (int k = 0; k < ops; ++k) {
          std::uint32_t i;
          if (r.dequeue(i)) r.enqueue(i);
        }
      });
    for (auto& t : ts) t.join();
    unsigned seen[8] = {}, n = 0;
    std::uint32_t i;
    while (r.dequeue(i)) {
      ++n;
      if (i < 8) ++seen[i];
    }
    bool ok = n == kThreads;
    for (unsigned k = 0; k < 8; ++k) ok = ok && seen[k] == (k < kThreads ? 1u : 0u);
    if (!ok) ++bad;
  }
  std::printf("ring (%s): %ld rounds, %ld with an index lost or doubled\n", kBuild, rounds, bad);
  EXPECT(bad == 0);
}
#endif

#ifndef WTC_TEST_EXPECT_NO_CAS2
// ---------------------------------------------------------------------------
// The thread slots.

void slots() {
  // Idle threads with hazard records of their own, from an unrelated queue.
  wtc::rt::segment_queue<int, false> other;
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::vector<std::thread> idle;
  for (int i = 0; i < 16; ++i)
    idle.emplace_back([&] {
      other.push(1);
      ready.fetch_add(1);
      while (!stop.load()) std::this_thread::yield();
    });
  while (ready.load() < 16) std::this_thread::yield();
  {
    using q_t = wtc::rt::index_queue<std::uint64_t, wtc::rt::no_ring_hooks, ring>;
    q_t q(16, true, 4);
    std::atomic<std::uint64_t> sum{0};
    std::vector<std::thread> ws;
    for (int t = 0; t < 4; ++t)
      ws.emplace_back([&, t] {
        for (std::uint64_t i = 1; i <= 20000; ++i) {
          if (t % 2 == 0) {
            while (!q.try_push(std::uint64_t(i))) std::this_thread::yield();
          } else {
            std::uint64_t v;
            while (!q.try_pop(v)) std::this_thread::yield();
            sum.fetch_add(v);
          }
        }
      });
    for (auto& w : ws) w.join();
    EXPECT(sum.load() == 2 * 20000ull * 20001 / 2);
    EXPECT(q.free_ring().fallbacks() == 0 && q.queued_ring().fallbacks() == 0);
    std::printf("slots: 16 idle threads with hazard records, 4 workers on a queue built for 4: fallbacks %llu\n",
                (unsigned long long)(q.free_ring().fallbacks() + q.queued_ring().fallbacks()));
  }
  stop = true;
  for (auto& t : idle) t.join();

  // Churn: a thread starts only after another has been joined.
  {
    ring_t r(3, 0, 4);
    std::vector<std::thread> ts(4);
    std::atomic<long> lost{0};
    for (long n = 0; n < 2000; ++n) {
      std::thread& t = ts[n % 4];
      if (t.joinable()) t.join();
      t = std::thread([&r, &lost, n] {
        r.enqueue(static_cast<std::uint32_t>(n % 4));
        std::uint32_t i;
        if (!r.dequeue(i)) lost.fetch_add(1);
      });
    }
    for (auto& t : ts) t.join();
    std::uint32_t i;
    EXPECT(!r.dequeue(i));
    EXPECT(lost.load() == 0);
    std::printf("slots: 2000 threads, at most 4 alive, on a ring built for 4: fallbacks %llu\n",
                (unsigned long long)r.fallbacks());
    EXPECT(r.fallbacks() == 0);
  }

#ifndef WTC_CHECK
  // More threads alive than the ring was built for: correct, and counted.
  {
    ring_t r(3, 0, 2);
    std::atomic<int> arrived{0};
    std::vector<std::thread> ts;
    std::atomic<long> got{0};
    for (unsigned t = 0; t < 4; ++t)
      ts.emplace_back([&, t] {
        r.enqueue(t);
        arrived.fetch_add(1);
        while (arrived.load() < 4) std::this_thread::yield();
        for (int k = 0; k < 5000; ++k) {
          std::uint32_t i;
          if (r.dequeue(i)) r.enqueue(i);
        }
        std::uint32_t i;
        if (r.dequeue(i)) got.fetch_add(1);
      });
    for (auto& t : ts) t.join();
    std::uint32_t i;
    while (r.dequeue(i)) got.fetch_add(1);
    EXPECT(got.load() == 4);
    std::printf("slots: 4 threads alive on a ring built for 2: fallbacks %llu\n", (unsigned long long)r.fallbacks());
    EXPECT(r.fallbacks() > 0);
  }
#endif
}

// Three threads alive on a ring built for two: the third has no slot.
void over() {
  ring_t r(3, 0, 2);
  std::atomic<int> arrived{0};
  std::vector<std::thread> ts;
  for (unsigned t = 0; t < 3; ++t)
    ts.emplace_back([&, t] {
      if (t == 2)
        while (arrived.load() < 2) std::this_thread::yield();
      r.enqueue(t);
      arrived.fetch_add(1);
      while (arrived.load() < 3) std::this_thread::yield();
    });
  for (auto& t : ts) t.join();
  std::printf("over: fallbacks %llu\n", (unsigned long long)r.fallbacks());
  EXPECT(r.fallbacks() == 1);
}

// A helper produces an enqueue's entry and stops before it resets the
// threshold, which is negative: the ring was built empty.
void helped() {
  wtc::rt::wcq_ring<hooks, wtc::rt::wide_words, wtc::rt::thread_slots, 0, 1, 1> r(1, 0, 2);
  // The owner takes slot 0 and stops at its first slow-path round.
  g_frozen.reset();
  std::thread owner([&] {
    t_freeze = kSlowRound;
    r.enqueue(0);
  });
  g_frozen.wait_arrival();
  std::thread helper([&] {
    // Its enqueue helps the owner's request first (help on every
    // operation), produces its entry and stops there.
    t_hold_produced = true;
    r.enqueue(1);
  });
  while (!g_produced_seen.load()) std::this_thread::yield();
  g_frozen.release();
  owner.join();
  // The owner's enqueue has returned with the helper still stopped.
  std::uint32_t i = 99;
  const bool got = r.dequeue(i);
  std::printf("helped: after the helped enqueue returned, a dequeue %s\n", got ? "found its element" : "answered empty");
  EXPECT(got && i == 0);
  g_produced_go.store(true);
  helper.join();
  unsigned n = 0;
  while (r.dequeue(i)) ++n;
  EXPECT(n == (got ? 1u : 2u));
}

// The late test's key: its destructor sets it again for four rounds and
// operates on the ring in the last.
pthread_key_t g_late_key;
ring_t* g_late_ring = nullptr;
void late_dtor(void* p) {
  const long round = reinterpret_cast<long>(p);
  if (round == 4) {
    g_late_ring->enqueue(1);
    std::uint32_t i;
    g_late_ring->dequeue(i);
  }
  if (round < 4) pthread_setspecific(g_late_key, reinterpret_cast<void*>(round + 1));
}

void late() {
  ring_t r(3, 0, 2);
  g_late_ring = &r;
  EXPECT(pthread_key_create(&g_late_key, late_dtor) == 0);
  for (int t = 0; t < 2; ++t)
    std::thread([] {
      g_late_ring->enqueue(0);
      std::uint32_t i;
      g_late_ring->dequeue(i);
      pthread_setspecific(g_late_key, reinterpret_cast<void*>(1));
    }).join();
  const std::uint64_t late_ops = r.fallbacks();
  std::thread([&] {
    for (int k = 0; k < 100; ++k) {
      r.enqueue(2);
      std::uint32_t i;
      r.dequeue(i);
    }
  }).join();
  std::printf("late: operations in the last key-destructor round fell back %llu times; a later thread %llu\n",
              (unsigned long long)late_ops, (unsigned long long)(r.fallbacks() - late_ops));
  EXPECT(late_ops == 4);  // their own, without a slot
  EXPECT(r.fallbacks() == late_ops);
}

// A fork while other threads hold slots, one stopped in a slow path.
using slow_ring = wtc::rt::wcq_ring<hooks, wtc::rt::wide_words, wtc::rt::thread_slots, 0, 1, 1>;
void forked() {
  slow_ring r(3, 0, 8);
  g_frozen.reset();
  // The first slot goes to a thread that stops in its enqueue's slow path,
  // with its request published.
  std::thread stopped([&] {
    t_freeze = kSlowRound;
    r.enqueue(7);
  });
  g_frozen.wait_arrival();
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::vector<std::thread> holders;
  for (unsigned t = 0; t < 7; ++t)
    holders.emplace_back([&, t] {
      r.enqueue(t);
      ready.fetch_add(1);
      while (!stop.load()) std::this_thread::yield();
    });
  while (ready.load() < 7) std::this_thread::yield();
  std::fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    // Every slot is held by a thread that is not here.
    int bad = 0;
    std::thread t([&] {
      // The holders' 0 to 6, and the stopped enqueue's 7, finished here if
      // no holder's help finished it before the fork.
      std::uint32_t seen[8] = {}, i, n = 0;
      while (r.dequeue(i)) {
        ++n;
        if (i < 8) ++seen[i];
      }
      for (unsigned k = 0; k < 8; ++k)
        if (seen[k] != 1) ++bad;
      if (n != 8) ++bad;
      if (r.fallbacks() != 0) ++bad;
      std::printf("fork: the child drained %u indices, each once: %s; fallbacks %llu\n", n, bad ? "no" : "yes",
                  (unsigned long long)r.fallbacks());
    });
    t.join();
    std::fflush(stdout);
    _exit(bad ? 1 : 0);
  }
  int status = 0;
  EXPECT(waitpid(pid, &status, 0) == pid);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  stop = true;
  for (auto& t : holders) t.join();
  g_frozen.release();
  stopped.join();
  std::uint32_t i;
  unsigned n = 0;
  while (r.dequeue(i)) ++n;
  EXPECT(n == 8);
}
#endif

void cas2() {
#if defined(__x86_64__)
  unsigned a, b, c, d;
  __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
  const bool cpu = c >> 13 & 1;
  std::printf("cas2: build %d, processor %d, supported %d\n", int(wtc::rt::kCas2WaitFree), int(cpu),
              int(wtc::rt::cas2_supported()));
  EXPECT(wtc::rt::cas2_supported() == (wtc::rt::kCas2WaitFree && cpu));
#ifdef WTC_TEST_EXPECT_NO_CAS2
  EXPECT(!wtc::rt::kCas2WaitFree);  // built without -mcx16
#else
  EXPECT(wtc::rt::kCas2WaitFree);
  queue q(4, false, 8);
  std::uint64_t v;
  EXPECT(q.try_push(std::uint64_t{5}) && q.try_pop(v) && v == 5);
#endif
#endif
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "frozen";
  const long n = argc > 2 ? std::atol(argv[2]) : 20000;
  (void)n;
  if (false) {
  }
#ifndef WTC_TEST_EXPECT_NO_CAS2
  else if (!std::strcmp(mode, "frozen")) frozen(n);
  else if (!std::strcmp(mode, "ring"))
    ring_churn(argc > 2 ? n : 1000, argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 6,
               argc > 4 ? std::atoi(argv[4]) : 2000);
  else if (!std::strcmp(mode, "slots")) slots();
  else if (!std::strcmp(mode, "over")) over();
  else if (!std::strcmp(mode, "fork")) forked();
  else if (!std::strcmp(mode, "helped")) helped();
  else if (!std::strcmp(mode, "late")) late();
#endif
  else if (!std::strcmp(mode, "cas2")) cas2();
  else {
    std::fprintf(stderr, "wcq_test: unknown mode %s\n", mode);
    return 2;
  }
  if (lemma::failures) {
    std::fprintf(stderr, "wcq_test %s: %d failure(s)\n", mode, lemma::failures.load());
    return 1;
  }
  std::printf("wcq_test %s: ok\n", mode);
  return 0;
}
