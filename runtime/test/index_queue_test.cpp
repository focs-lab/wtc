// Tests of the bounded lock-free queue over rings of indices
// (runtime/index_ring.hpp), both variants.
//
//   index_queue_test model [N]   one thread, random operations against a
//                                sequential bounded queue: every answer,
//                                and the exact variant's size
//   index_queue_test lin [R]     R rounds of three threads, four
//                                operations each, at capacities 1 to 3,
//                                pausing at random at the hooks: every
//                                history linearisable as a FIFO queue
//                                that never holds more than its capacity
//                                (searched exhaustively), and every
//                                refused push preceded by at least the
//                                capacity's worth of pushes not yet
//                                popped
//   index_queue_test held        a pop held while it moves its element
//                                out of a full queue: the exact variant
//                                takes a push in the room the pop made,
//                                the plain one refuses it
//   index_queue_test frozen      a producer frozen after taking its cell,
//                                and one frozen inside the ring's
//                                enqueue, while the other producers and
//                                the consumer go on: the consumer takes
//                                every other element, with one consumer
//                                and with several; and a consumer frozen
//                                with its cell while the others go on
//   index_queue_test stress [N]  four producers and four consumers, and
//                                four producers and one consumer: every
//                                element once, each producer's order at
//                                each consumer, every empty answer checked
//   index_queue_test boundary    the exact variant built for T threads,
//                                T - 1 pops held inside their moves, and
//                                one thread pushing it back to its
//                                capacity: every push up to the capacity
//                                finds a cell, and the next is refused
//   index_queue_test inflight    the history the exact variant admits and
//                                a linearisable bounded queue does not
//                                (index_ring.hpp, "Capacity"): capacity 1,
//                                a push stopped after its reservation, and
//                                on one thread a refused push and then an
//                                empty pop
//   index_queue_test ring [R]    the ring alone, R rounds: a ring of two
//                                indices built full, six threads each
//                                dequeuing an index and enqueuing it back
//                                3000 times, yielding at random after
//                                taking positions and while holding an
//                                index; then a drain on one
//                                thread finds each index once. A threshold
//                                too small to cover the dequeues that can
//                                run ahead of an enqueue makes that drain
//                                answer empty early
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "index_ring.hpp"
#include "lemma_util.hpp"

namespace {

// Hooks: a thread holds at a point while it has a gate for it; in the
// linearisability rounds each point yields at random.
enum point { kEnq, kDeq, kPushCell, kPopCell, kPoints };
thread_local lemma::gate* t_hold[kPoints] = {};
thread_local unsigned t_jitter = 0;
// The ring churn's jitter: a yield one time in seven, otherwise now and
// then a spin of up to 200 pauses.
thread_local std::uint64_t t_churn = 0;
void churn_jitter() {
  t_churn ^= t_churn << 13;
  t_churn ^= t_churn >> 7;
  t_churn ^= t_churn << 17;
  if (t_churn % 7 == 0) {
    std::this_thread::yield();
  } else if (t_churn % 31 == 1) {
    for (unsigned k = 0; k < (t_churn >> 40) % 200; ++k) std::atomic_signal_fence(std::memory_order_seq_cst);
  }
}
void at(point p) {
  if (lemma::gate* g = t_hold[p]) {
    t_hold[p] = nullptr;
    g->stop_here();
    return;
  }
  if (t_churn) churn_jitter();
  if (t_jitter) {
    t_jitter = t_jitter * 1103515245u + 12345u;
    if ((t_jitter >> 16) % 3 == 0) std::this_thread::yield();
  }
}
struct hooks : wtc::rt::no_ring_hooks {
  static void enq_reserved() { at(kEnq); }
  static void deq_reserved() { at(kDeq); }
  static void push_has_cell() { at(kPushCell); }
  static void pop_has_cell() { at(kPopCell); }
};

// Not trivially movable, so that a move that went wrong shows.
struct elem {
  std::uint64_t v = 0;
  std::string pad;
  elem() = default;
  explicit elem(std::uint64_t x) : v(x), pad(20, char('a' + x % 26)) {}
  bool intact() const { return pad.size() == 20 && pad[0] == char('a' + v % 26); }
};

// The ring under test: SCQ (lock-free), or with WTC_TEST_WCQ the
// wait-free ring, 1 with its defaults, 2 with no patience for enqueues,
// one try for dequeues and help on every operation, so that the slow path
// and the helping run all the time.
#if WTC_TEST_WCQ == 1
template <class H>
using test_ring = wtc::rt::wcq_ring<H>;
#elif WTC_TEST_WCQ == 2
template <class H>
using test_ring = wtc::rt::wcq_ring<H, wtc::rt::wide_words, wtc::rt::thread_slots, 0, 1, 1>;
#else
template <class H>
using test_ring = wtc::rt::scq_ring<H>;
#endif

// Every queue is built for the threads alive at once that use it, the
// main thread's included where it does: the wait-free ring's thread slots
// are its own (index_ring.hpp), so the other threads of the process do
// not count.
using queue = wtc::rt::index_queue<elem, hooks, test_ring>;

void model(long n) {
  std::mt19937_64 rng(7);
  for (bool exact : {false, true})
    for (unsigned threads : {1u, 4u, 128u})
      for (std::size_t cap = 0; cap <= 6; ++cap) {
        queue q(cap, exact, threads);
        std::deque<std::uint64_t> m;
        std::uint64_t next = 0;
        for (long i = 0; i < n; ++i) {
          if (rng() % 2) {
            const bool ok = q.try_push(elem(next));
            EXPECT(ok == (m.size() < cap));
            if (ok) m.push_back(next);
            ++next;
          } else {
            elem out;
            const bool ok = q.try_pop(out);
            EXPECT(ok == !m.empty());
            if (ok) {
              EXPECT(out.v == m.front() && out.intact());
              m.pop_front();
            }
          }
          if (exact) EXPECT(q.size() == m.size());
        }
      }
  std::printf("model: %s\n", lemma::failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// lin: histories checked by search.

std::atomic<std::uint64_t> g_clock{0};

struct op {
  enum kind { push_ok, push_refused, pop_ok, pop_empty } k;
  std::uint64_t v = 0;
  std::uint64_t inv = 0, res = 0;
};

// Is there an order of ops, consistent with real time, that a FIFO queue
// holding at most cap elements gives? Refused pushes are checked apart.
bool linearisable(const std::vector<op>& ops, std::size_t cap) {
  const std::size_t n = ops.size();
  std::vector<char> done(n, 0);
  std::deque<std::uint64_t> q;
  // Depth-first over the ops that may come next: those invoked before
  // every remaining op has returned.
  struct rec {
    static bool go(const std::vector<op>& ops, std::vector<char>& done, std::deque<std::uint64_t>& q,
                   std::size_t left, std::size_t cap) {
      if (left == 0) return true;
      std::uint64_t min_res = ~std::uint64_t{0};
      for (std::size_t i = 0; i < ops.size(); ++i)
        if (!done[i] && ops[i].res < min_res) min_res = ops[i].res;
      for (std::size_t i = 0; i < ops.size(); ++i) {
        if (done[i] || ops[i].inv > min_res) continue;
        const op& o = ops[i];
        done[i] = 1;
        bool ok = false;
        switch (o.k) {
        case op::push_ok:
          if (q.size() < cap) {
            q.push_back(o.v);
            ok = go(ops, done, q, left - 1, cap);
            q.pop_back();
          }
          break;
        case op::push_refused:
          ok = go(ops, done, q, left - 1, cap);
          break;
        case op::pop_ok:
          if (!q.empty() && q.front() == o.v) {
            q.pop_front();
            ok = go(ops, done, q, left - 1, cap);
            q.push_front(o.v);
          }
          break;
        case op::pop_empty:
          if (q.empty()) ok = go(ops, done, q, left - 1, cap);
          break;
        }
        done[i] = 0;
        if (ok) return true;
      }
      return false;
    }
  };
  return rec::go(ops, done, q, n, cap);
}

// A refused push R: the queue was full at a point inside R only if the
// pushes that may have taken effect before R returned, less the pops that
// had returned before R began, are at least the capacity.
bool refusals_justified(const std::vector<op>& ops, std::size_t cap) {
  for (const op& r : ops) {
    if (r.k != op::push_refused) continue;
    long pushed = 0, popped = 0;
    for (const op& o : ops) {
      if (o.k == op::push_ok && o.inv < r.res) ++pushed;
      if (o.k == op::pop_ok && o.res < r.inv) ++popped;
    }
    if (pushed - popped < static_cast<long>(cap)) return false;
  }
  return true;
}

void lin(long rounds) {
  long checked = 0, bad = 0;
  for (long round = 0; round < rounds; ++round) {
    for (bool exact : {false, true}) {
      const std::size_t cap = 1 + static_cast<std::size_t>(round % 3);
      queue q(cap, exact, 4);
      std::vector<op> ops[3];
      std::atomic<int> ready{0};
      std::vector<std::thread> ts;
      for (int t = 0; t < 3; ++t)
        ts.emplace_back([&, t] {
          std::mt19937 r(static_cast<unsigned>(round * 7 + t * 131 + (exact ? 1 : 0)));
          t_jitter = static_cast<unsigned>(r()) | 1u;
          ready.fetch_add(1);
          while (ready.load() < 3) {
          }
          for (int k = 0; k < 4; ++k) {
            op o;
            if (r() % 2) {
              o.v = static_cast<std::uint64_t>(t * 100 + k + 1);
              o.inv = g_clock.fetch_add(1);
              const bool ok = q.try_push(elem(o.v));
              o.res = g_clock.fetch_add(1);
              o.k = ok ? op::push_ok : op::push_refused;
            } else {
              elem out;
              o.inv = g_clock.fetch_add(1);
              const bool ok = q.try_pop(out);
              o.res = g_clock.fetch_add(1);
              o.k = ok ? op::pop_ok : op::pop_empty;
              o.v = out.v;
              if (ok) EXPECT(out.intact());
            }
            ops[t].push_back(o);
          }
          t_jitter = 0;
        });
      for (auto& t : ts) t.join();
      std::vector<op> all;
      for (auto& v : ops) all.insert(all.end(), v.begin(), v.end());
      ++checked;
      if (!linearisable(all, cap) || !refusals_justified(all, cap)) {
        ++bad;
        if (bad <= 3) {
          std::fprintf(stderr, "lin: round %ld (%s, capacity %zu) not linearisable:\n", round,
                       exact ? "exact" : "plain", cap);
          for (const op& o : all)
            std::fprintf(stderr, "  [%llu, %llu] %s %llu\n", (unsigned long long)o.inv,
                         (unsigned long long)o.res,
                         o.k == op::push_ok ? "push" : o.k == op::push_refused ? "push refused"
                                                    : o.k == op::pop_ok        ? "pop"
                                                                               : "pop empty",
                         (unsigned long long)o.v);
        }
      }
    }
  }
  std::printf("lin: %ld histories, %ld not linearisable\n", checked, bad);
  EXPECT(bad == 0);
}

// ---------------------------------------------------------------------------
// held: a pop held inside its move out of a full queue.

void held() {
  for (bool exact : {false, true}) {
    queue q(2, exact, 4);
    EXPECT(q.try_push(elem(1)));
    EXPECT(q.try_push(elem(2)));
    EXPECT(!q.try_push(elem(3)));
    lemma::gate g;
    std::thread c([&] {
      t_hold[kPopCell] = &g;
      elem out;
      EXPECT(q.try_pop(out) && out.v == 1);
    });
    g.wait_arrival();
    // The held pop has dequeued its index: the element has left the
    // queue. The exact variant has room for one, as the mutex ring does;
    // the plain one has no free cell until the pop gives its back.
    const bool ok = q.try_push(elem(3));
    EXPECT(ok == exact);
    if (exact) {
      EXPECT(q.size() == 2);
      EXPECT(!q.try_push(elem(4)));  // and now it is full
    }
    g.release();
    c.join();
    elem out;
    EXPECT(q.try_pop(out) && out.v == 2);
    if (exact) EXPECT(q.try_pop(out) && out.v == 3);
    EXPECT(!q.try_pop(out));
    std::printf("held (%s): push while a pop moves out of a full queue %s\n", exact ? "exact" : "plain",
                ok ? "taken" : "refused");
  }
}

// ---------------------------------------------------------------------------
// frozen: threads stopped inside their operations stop nobody else.

void frozen_producers(unsigned consumers) {
  for (point where : {kPushCell, kEnq}) {
    for (bool exact : {false, true}) {
      queue q(64, exact, 8);
      lemma::gate g;
      std::thread stuck([&] {
        t_hold[where] = &g;
        EXPECT(q.try_push(elem(999999)));
      });
      g.wait_arrival();
      // Another producer pushes, the consumers take: the stopped push holds
      // a cell (or a position of aq) and nothing else.
      const std::uint64_t n = 20000;
      std::atomic<std::uint64_t> got{0}, sum{0};
      std::atomic<bool> done{false};
      std::thread prod([&] {
        for (std::uint64_t i = 1; i <= n; ++i)
          while (!q.try_push(elem(i))) std::this_thread::yield();
        done = true;
      });
      std::vector<std::thread> cs;
      for (unsigned c = 0; c < consumers; ++c)
        cs.emplace_back([&] {
          elem out;
          while (got.load() < n) {
            if (q.try_pop(out)) {
              EXPECT(out.v != 999999 && out.intact());
              sum.fetch_add(out.v);
              got.fetch_add(1);
            }
          }
        });
      prod.join();
      for (auto& t : cs) t.join();
      EXPECT(got.load() == n);
      EXPECT(sum.load() == n * (n + 1) / 2);
      g.release();
      stuck.join();
      elem out;
      EXPECT(q.try_pop(out) && out.v == 999999);
      EXPECT(!q.try_pop(out));
      std::printf("frozen producer at %s (%s, %u consumer%s): the others' %llu elements taken\n",
                  where == kPushCell ? "its cell" : "the ring's enqueue", exact ? "exact" : "plain",
                  consumers, consumers == 1 ? "" : "s", (unsigned long long)got.load());
    }
  }
}

void frozen_consumer() {
  for (point where : {kDeq, kPopCell}) {
    queue q(16, true, 8);
    for (std::uint64_t i = 1; i <= 8; ++i) EXPECT(q.try_push(elem(i)));
    lemma::gate g;
    std::atomic<std::uint64_t> held_v{0};
    std::thread stuck([&] {
      t_hold[where] = &g;
      elem out;
      if (q.try_pop(out)) held_v = out.v;
    });
    g.wait_arrival();
    std::uint64_t got = 0;
    elem out;
    std::thread other([&] {
      for (std::uint64_t i = 9; i <= 1000; ++i)
        while (!q.try_push(elem(i))) std::this_thread::yield();
    });
    // The held pop owns at most one element (the one at its position, or
    // in its cell); every other one is taken past it. On the lock-free
    // ring and the wait-free one's fast path that is the first element; a
    // slow-path enqueue may leave the first position empty.
    while (got < 999)
      if (q.try_pop(out)) ++got;
    other.join();
    if (q.try_pop(out)) ++got;
    EXPECT(!q.try_pop(out));
    g.release();
    stuck.join();
    EXPECT(got + (held_v.load() ? 1 : 0) == 1000);
#if !defined(WTC_TEST_WCQ) || WTC_TEST_WCQ == 1
    EXPECT(held_v.load() == 1);
#endif
    std::printf("frozen consumer at %s: the others took %llu\n", where == kDeq ? "the ring's dequeue" : "its cell",
                (unsigned long long)got);
  }
}

void frozen() {
  frozen_producers(1);
  frozen_producers(3);
  frozen_consumer();
}

// ---------------------------------------------------------------------------
// boundary: the exact variant's spare cells, all in use at once.

void boundary() {
  for (unsigned threads : {2u, 4u, 8u}) {
    const std::size_t cap = threads;
    queue q(cap, true, threads);
    for (std::size_t i = 0; i < cap; ++i) EXPECT(q.try_push(elem(i)));
    std::vector<lemma::gate> gates(threads - 1);
    std::vector<std::thread> pops;
    for (unsigned k = 0; k + 1 < threads; ++k)
      pops.emplace_back([&, k] {
        t_hold[kPopCell] = &gates[k];
        elem out;
        EXPECT(q.try_pop(out));
      });
    for (auto& g : gates) g.wait_arrival();
    // The held pops have given their count back and still hold their
    // cells: the pushes back to the capacity need every spare cell.
    for (unsigned k = 0; k + 1 < threads; ++k) EXPECT(q.try_push(elem(100 + k)));
    EXPECT(!q.try_push(elem(999)));
    for (auto& g : gates) g.release();
    for (auto& t : pops) t.join();
    std::size_t n = 0;
    elem out;
    while (q.try_pop(out)) ++n;
    EXPECT(n == cap);
    std::printf("boundary: %u threads, %u pops held in their moves, the queue pushed back to its capacity\n", threads,
                threads - 1);
  }
}

// inflight: a push stopped after its reservation, at capacity 1.

void inflight() {
  queue q(1, true, 2);
  lemma::gate g;
  std::thread p([&] {
    t_hold[kPushCell] = &g;
    EXPECT(q.try_push(elem(7)));
  });
  g.wait_arrival();
  // The answers the header states: the reservation counts for the push,
  // and the element is not there for the pop.
  EXPECT(!q.try_push(elem(8)));
  elem out;
  EXPECT(!q.try_pop(out));
  g.release();
  p.join();
  EXPECT(q.try_pop(out) && out.v == 7);
  std::printf("inflight: a push stopped after its reservation; a push refused, then a pop empty\n");
}

// ---------------------------------------------------------------------------
// ring: the ring alone under churn, then drained by one thread.

void ring_churn(long rounds) {
  long bad = 0;
  constexpr unsigned kThreads = 6;
  for (long round = 0; round < rounds; ++round) {
    test_ring<hooks> r(1, 2, kThreads);
    std::atomic<unsigned> go{0};
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < kThreads; ++t)
      ts.emplace_back([&, t] {
        t_churn = (static_cast<std::uint64_t>(round) * 1000003u + t * 7919u + 17) | 1u;
        go.fetch_add(1);
        while (go.load() < kThreads) {
        }
        for (int k = 0; k < 3000; ++k) {
          std::uint32_t i;
          if (!r.dequeue(i)) continue;
          // Held for a while now and then, so that the others' dequeues
          // fail and run ahead.
          churn_jitter();
          if (t_churn % 4 == 0) std::this_thread::yield();
          r.enqueue(i);
        }
        t_churn = 0;
      });
    for (auto& t : ts) t.join();
    unsigned seen[2] = {}, n = 0;
    std::uint32_t i;
    while (r.dequeue(i)) {
      ++n;
      if (i < 2) ++seen[i];
    }
    if (n != 2 || seen[0] != 1 || seen[1] != 1) {
      if (++bad <= 3)
        std::fprintf(stderr, "ring: round %ld: drained %u indices (%u of 0, %u of 1)\n", round, n, seen[0], seen[1]);
    }
  }
  std::printf("ring: %ld rounds, %ld with an index lost, doubled or not found\n", rounds, bad);
  EXPECT(bad == 0);
}

// ---------------------------------------------------------------------------
// stress

void stress(long per_producer) {
  for (unsigned consumers : {4u, 1u})
    for (bool exact : {false, true}) {
      const unsigned producers = 4;
      queue q(8, exact, producers + consumers);
      lemma::empty_check check;
      std::atomic<long> popped{0};
      std::atomic<unsigned> producers_left{producers};
      std::vector<std::thread> ts;
      for (unsigned p = 0; p < producers; ++p)
        ts.emplace_back([&, p] {
          for (long s = 0; s < per_producer; ++s) {
            while (!q.try_push(elem(std::uint64_t{p} << 40 | static_cast<std::uint64_t>(s))))
              std::this_thread::yield();
            check.pushed();
          }
          producers_left.fetch_sub(1);
        });
      for (unsigned c = 0; c < consumers; ++c)
        ts.emplace_back([&] {
          std::vector<long> last(producers, -1);
          elem out;
          for (;;) {
            const bool got = check.pop([&] { return q.try_pop(out); });
            if (got) {
              EXPECT(out.intact());
              const unsigned p = static_cast<unsigned>(out.v >> 40);
              const long s = static_cast<long>(out.v & ((std::uint64_t{1} << 40) - 1));
              EXPECT(s > last[p]);
              last[p] = s;
              popped.fetch_add(1);
            } else if (producers_left.load() == 0 && popped.load() == producers * per_producer) {
              break;
            }
          }
        });
      for (auto& t : ts) t.join();
      EXPECT(popped.load() == producers * per_producer);
      EXPECT(check.violations.load() == 0);
      std::printf("stress (%s, 4 producers, %u consumers): %ld elements\n", exact ? "exact" : "plain",
                  consumers, popped.load());
    }
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "model";
  const long n = argc > 2 ? std::atol(argv[2]) : 0;
  if (!std::strcmp(mode, "model")) model(n ? n : 20000);
  else if (!std::strcmp(mode, "lin")) lin(n ? n : 20000);
  else if (!std::strcmp(mode, "held")) held();
  else if (!std::strcmp(mode, "frozen")) frozen();
  else if (!std::strcmp(mode, "stress")) stress(n ? n : 200000);
  else if (!std::strcmp(mode, "boundary")) boundary();
  else if (!std::strcmp(mode, "inflight")) inflight();
  else if (!std::strcmp(mode, "ring")) ring_churn(n ? n : 2000);
  else {
    std::fprintf(stderr, "index_queue_test: unknown mode %s\n", mode);
    return 2;
  }
  if (lemma::failures) {
    std::fprintf(stderr, "index_queue_test %s: %d failure(s)\n", mode, lemma::failures.load());
    return 1;
  }
  std::printf("index_queue_test %s: ok\n", mode);
  return 0;
}
