// Tests of the runtime of lifted C queues (shim/c_queue.h, c_queue.cpp),
// through its C entry points, as a lifted program calls them.
//
//   c_queue_test model [N]   random operations, one thread, against the
//                            original: a ring of `slots` pointers with its
//                            end count, and the caller's own rings; every
//                            answer, the queue's contents and the caller's
//                            rings' indices
//   c_queue_test stress [N]  producers that push single elements and
//                            batches from rings of their own, one consumer
//                            that pops batches, at capacities 1 to 21: every
//                            element arrives once, in order per producer,
//                            every batch pushed arrives as adjacent
//                            elements, and the queue finishes once every
//                            producer has terminated
//   c_queue_test consumers [N] [REPS]  four producers pushing single
//                            elements and batches, four consumers popping
//                            batches with random limits into rings with
//                            random room: every element arrives exactly
//                            once, every answer is within the limit and the
//                            room, and there is one order of all elements,
//                            each producer's in turn with its batches
//                            adjacent, of which every batch popped is an
//                            adjacent run, each consumer's in turn
//   c_queue_test close       consumers asleep on an empty queue wake with
//                            "finished" when the last producer terminates,
//                            and not before
//   c_queue_test crossing    (built with WTC_TEST_BATCH_STEPS) a batched
//                            pop held between deciding its size and
//                            claiming its tickets while a batched push runs
//                            whole, and the other way round: two elements
//                            queued, capacity four, both asking for ten;
//                            the sizes must be an outcome of the original's
//                            mutex, two and four or four and two
//   c_queue_test twoconsumers  two consumers on a queue built for one, which
//                            the contract check must catch
//   c_queue_test overterminate  one termination more than the producers,
//                            which must end the program when the queue
//                            finishes at "exactly"
//   c_queue_test atleast     the same at "at least": nothing changes
//   c_queue_test limit       a limit beyond any int is no limit
//   c_queue_test absorb      a batch with nothing to move passes on the
//                            wake-up it took
//   c_queue_test destroy     (built with WTC_C_QUEUE_TEST_HOOK) destroy
//                            waits for an operation still in the runtime

#include "../../shim/c_queue.h"
#include "../../shim/topology.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using wtc::topology;

std::atomic<int> g_failures{0};
#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// The program's queue object, as ferret's struct queue: the runtime keeps
// its pointer in the first eight bytes.
struct object {
  void* storage[8];
};

// A ring of the caller's, as dedup's ringbuffer_t.
struct own {
  int head = 0, tail = 0;
  void** data;
  unsigned long size;
  explicit own(unsigned long slots) : data(new void*[slots]), size(slots) {}
  ~own() { delete[] data; }
  own(const own&) = delete;
  own& operator=(const own&) = delete;
  bool empty() const { return head == tail; }
  bool full() const { return static_cast<unsigned long>(head) == (tail - 1 + size) % size; }
  unsigned long count() const { return (head + size - tail) % size; }
  void insert(void* v) {
    data[head] = v;
    head = static_cast<int>((head + 1) % size);
  }
  void* remove() {
    void* v = data[tail];
    tail = static_cast<int>((tail + 1) % size);
    return v;
  }
};

// The limit as the lifted program passes it: a signed one below zero is
// none (LiftCQueues.cpp).
int push_batch(object& q, own& r, long limit) {
  return wtc_c_queue_push_batch(&q, &r.data, &r.head, &r.tail, r.size, limit < 0 ? 0 : limit);
}
int pop_batch(object& q, own& r, long limit) {
  return wtc_c_queue_pop_batch(&q, &r.data, &r.head, &r.tail, r.size, limit < 0 ? 0 : limit);
}
// How a queue finishes (shim/c_queue.h).
constexpr int kNever = 0, kExactly = 1, kAtLeast = 2;

void* as_ptr(std::uint64_t v) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(v)); }
std::uint64_t as_int(void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); }

// ---------------------------------------------------------------------------
// model: the original's answers, operation by operation, where they do not
// wait.

void model(long n) {
  std::mt19937_64 rng(7);
  for (topology tag : {topology::spsc, topology::mpsc, topology::spmc, topology::generic})
    for (long cap = 0; cap <= 6; ++cap)
      for (int producers = 0; producers <= 2; ++producers)
       for (int ends : {kExactly, kAtLeast}) {
        object q;
        wtc_c_queue_init(&q, static_cast<int>(tag), cap, producers, ends);
        std::deque<std::uint64_t> m;
        int ended = 0;
        std::uint64_t next = 1;
        own src(1 + rng() % 6), dst(1 + rng() % 6);
        std::deque<std::uint64_t> srcM, dstM;
        for (long i = 0; i < n; ++i) {
          const bool full = static_cast<long>(m.size()) >= cap;
          const bool finished = ended >= producers;
          switch (rng() % 7) {
          case 0:
            if (!full) {
              wtc_c_queue_push_wait(&q, as_ptr(next));
              m.push_back(next++);
            }
            break;
          case 1:
            if (!m.empty() || finished) {
              void* out = nullptr;
              const int r = wtc_c_queue_pop_wait(&q, &out);
              EXPECT(r == (m.empty() ? 0 : 1));
              if (r) {
                EXPECT(as_int(out) == m.front());
                m.pop_front();
              }
            }
            break;
          case 2:  // fill the caller's source ring
            while (!src.full() && rng() % 3) {
              src.insert(as_ptr(next));
              srcM.push_back(next++);
            }
            break;
          case 3:
            if (!full) {
              const int limit = static_cast<int>(rng() % 5) - 1;
              int want = 0;
              while (limit > 0 && want < limit && want < static_cast<int>(srcM.size()) &&
                     static_cast<long>(m.size()) + want < cap)
                ++want;
              const int r = push_batch(q, src, limit);
              EXPECT(r == want);
              for (int j = 0; j < r; ++j) {
                m.push_back(srcM.front());
                srcM.pop_front();
              }
              EXPECT(src.count() == srcM.size());
            }
            break;
          case 4:
            if (!m.empty() || finished) {
              const int limit = static_cast<int>(rng() % 5) - 1;
              const int r = pop_batch(q, dst, limit);
              if (m.empty()) {
                EXPECT(r == -1);
              } else {
                int want = 0;
                while (limit > 0 && want < limit && want < static_cast<int>(m.size()) &&
                       dstM.size() + want < dst.size - 1)
                  ++want;
                EXPECT(r == want);
                for (int j = 0; j < r && j < want; ++j) {
                  dstM.push_back(m.front());
                  m.pop_front();
                }
              }
              EXPECT(dst.count() == dstM.size());
            }
            break;
          case 5:  // drain the caller's destination ring
            while (!dst.empty() && rng() % 3) {
              EXPECT(as_int(dst.remove()) == dstM.front());
              dstM.pop_front();
            }
            break;
          case 6:
            // At least: terminations beyond the producers change nothing.
            if ((ended < producers || ends == kAtLeast) && rng() % 50 == 0) {
              wtc_c_queue_terminate(&q);
              ++ended;
            }
            break;
          }
        }
        wtc_c_queue_destroy(&q);
      }
  std::printf("model: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// stress: an element is producer << 48 | batch << 24 | index, plus one, so
// that none is null; a single push is a batch of one.

void stress(long n) {
  for (long cap : {1L, 2L, 3L, 20L})
    for (int producers : {1, 2, 4}) {
      const topology tag = producers == 1 ? topology::spsc : topology::mpsc;
      object q;
      wtc_c_queue_init(&q, static_cast<int>(tag), cap, producers, kExactly);
      std::vector<std::thread> ts;
      for (int p = 0; p < producers; ++p)
        ts.emplace_back([&, p] {
          std::mt19937 rng(p + 1);
          own r(21);
          std::uint64_t batch = 0;
          for (long sent = 0; sent < n;) {
            ++batch;
            if (rng() % 4 == 0) {
              wtc_c_queue_push_wait(&q, as_ptr((std::uint64_t(p) << 48 | batch << 24) + 1));
              ++sent;
              continue;
            }
            int k = 1 + static_cast<int>(rng() % 20);
            if (k > n - sent) k = static_cast<int>(n - sent);
            for (int j = 0; j < k; ++j) r.insert(nullptr);
            // Each call's run must arrive adjacent: what is left in the
            // ring is numbered afresh before every call.
            for (bool first = true; !r.empty(); first = false) {
              if (!first) ++batch;
              for (unsigned long j = 0, c = r.count(); j < c; ++j)
                r.data[(r.tail + j) % r.size] = as_ptr((std::uint64_t(p) << 48 | batch << 24 | j) + 1);
              sent += push_batch(q, r, 20);
            }
          }
          wtc_c_queue_terminate(&q);
        });
      std::vector<std::uint64_t> seen;
      own d(21);
      for (;;) {
        const int r = pop_batch(q, d, 20);
        if (r < 0) break;
        while (!d.empty()) seen.push_back(as_int(d.remove()) - 1);
      }
      for (auto& t : ts) t.join();
      // Per producer, in order; each batch adjacent and complete in turn.
      std::vector<std::uint64_t> last(producers, 0);
      std::vector<long> count(producers, 0);
      for (std::size_t i = 0; i < seen.size(); ++i) {
        const std::uint64_t v = seen[i];
        const int p = static_cast<int>(v >> 48);
        const std::uint64_t key = v & ((std::uint64_t{1} << 48) - 1);
        EXPECT(p >= 0 && p < producers);
        if (p < 0 || p >= producers) continue;
        EXPECT(key > last[p] || count[p] == 0);
        last[p] = key;
        ++count[p];
        const std::uint64_t idx = v & 0xffffff;
        if (idx > 0) {
          EXPECT(i > 0);
          if (i > 0) EXPECT(seen[i - 1] == v - 1);  // the batch's previous element, adjacent
        }
      }
      for (int p = 0; p < producers; ++p) EXPECT(count[p] == n);
      wtc_c_queue_destroy(&q);
    }
  std::printf("stress: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// consumers: several consumers popping batches. An element is
// (producer + 1) << 40 | sequence number, from 1. What is checked after
// the threads have joined, without seeing the queue's tickets: that some
// order of all the elements exists in which each producer's elements come
// in turn, each batch it pushed is adjacent, every batch a consumer popped
// is an adjacent run, and each consumer's batches come in turn. The batches
// popped are the nodes of a graph: the next element of a producer in a
// later batch is an edge, a batch pushed that one popped batch ends and the
// next begins ties the two together, and the order must be acyclic.

struct popped {
  int consumer;
  std::vector<std::uint64_t> elems;
};

bool ordered(const std::vector<popped>& batches, const std::vector<std::vector<std::pair<std::uint64_t, int>>>& pushed,
             int producers, long n, std::string& why) {
  const std::size_t B = batches.size();
  // Where each element went: batch and position.
  std::vector<std::vector<std::pair<int, int>>> at(producers, std::vector<std::pair<int, int>>(n + 1, {-1, -1}));
  for (std::size_t b = 0; b < B; ++b)
    for (std::size_t i = 0; i < batches[b].elems.size(); ++i) {
      const std::uint64_t v = batches[b].elems[i];
      const long p = static_cast<long>(v >> 40) - 1;
      const long s = static_cast<long>(v & ((std::uint64_t{1} << 40) - 1));
      if (p < 0 || p >= producers || s < 1 || s > n) {
        why = "an element nobody pushed";
        return false;
      }
      if (at[p][s].first != -1) {
        why = "an element popped twice";
        return false;
      }
      at[p][s] = {static_cast<int>(b), static_cast<int>(i)};
    }
  for (int p = 0; p < producers; ++p)
    for (long s = 1; s <= n; ++s)
      if (at[p][s].first == -1) {
        why = "an element lost";
        return false;
      }
  std::vector<std::vector<int>> succ(B);
  std::vector<int> next(B, -1), prev(B, -1);
  auto edge = [&](int a, int b) { succ[a].push_back(b); };
  // Each producer's elements in turn.
  for (int p = 0; p < producers; ++p)
    for (long s = 1; s < n; ++s) {
      const auto x = at[p][s], y = at[p][s + 1];
      if (x.first == y.first) {
        if (y.second < x.second) {
          why = "a producer's elements out of order within a batch";
          return false;
        }
      } else {
        edge(x.first, y.first);
      }
    }
  // Each batch pushed adjacent.
  for (int p = 0; p < producers; ++p)
    for (const auto& [first, k] : pushed[p])
      for (long s = static_cast<long>(first); s + 1 < static_cast<long>(first) + k; ++s) {
        const auto x = at[p][s], y = at[p][s + 1];
        if (x.first == y.first) {
          if (y.second != x.second + 1) {
            why = "a batch pushed split inside a batch popped";
            return false;
          }
          continue;
        }
        if (x.second + 1 != static_cast<int>(batches[x.first].elems.size()) || y.second != 0 ||
            (next[x.first] != -1 && next[x.first] != y.first) || (prev[y.first] != -1 && prev[y.first] != x.first)) {
          why = "a batch pushed split across batches popped that are not adjacent";
          return false;
        }
        next[x.first] = y.first;
        prev[y.first] = x.first;
      }
  // Each consumer's batches in turn.
  std::vector<int> last(8, -1);
  for (std::size_t b = 0; b < B; ++b) {
    const int c = batches[b].consumer;
    if (last[c] != -1) edge(last[c], static_cast<int>(b));
    last[c] = static_cast<int>(b);
  }
  // Batches tied by a batch pushed are one node, in their order.
  std::vector<int> chain(B, -1), rank(B, 0);
  int chains = 0;
  for (std::size_t b = 0; b < B; ++b) {
    if (prev[b] != -1) continue;
    int r = 0;
    for (int x = static_cast<int>(b); x != -1; x = next[x]) {
      chain[x] = chains;
      rank[x] = r++;
    }
    ++chains;
  }
  for (std::size_t b = 0; b < B; ++b)
    if (chain[b] == -1) {
      why = "batches popped tied in a cycle";
      return false;
    }
  std::vector<std::vector<int>> csucc(chains);
  std::vector<int> indeg(chains, 0);
  for (std::size_t a = 0; a < B; ++a)
    for (int b : succ[a]) {
      if (chain[a] == chain[b]) {
        if (rank[b] <= rank[a]) {
          why = "an order against batches tied by a batch pushed";
          return false;
        }
        continue;
      }
      csucc[chain[a]].push_back(chain[b]);
      ++indeg[chain[b]];
    }
  std::vector<int> ready;
  for (int c = 0; c < chains; ++c)
    if (indeg[c] == 0) ready.push_back(c);
  int done = 0;
  while (!ready.empty()) {
    const int c = ready.back();
    ready.pop_back();
    ++done;
    for (int d : csucc[c])
      if (--indeg[d] == 0) ready.push_back(d);
  }
  if (done != chains) {
    why = "no order of the elements makes every batch popped an adjacent run";
    return false;
  }
  return true;
}

void consumers_test(long n, long reps) {
  constexpr int kProducers = 4, kConsumers = 4;
  const long caps[] = {1, 2, 3, 5, 8, 20, 64};
  long batches_total = 0, multi = 0;
  for (long rep = 0; rep < reps; ++rep) {
    const long cap = caps[rep % (sizeof caps / sizeof caps[0])];
    object q;
    wtc_c_queue_init(&q, static_cast<int>(topology::generic), cap, kProducers, kExactly);
    std::vector<std::vector<std::pair<std::uint64_t, int>>> pushed(kProducers);
    std::vector<std::vector<popped>> got(kConsumers);
    std::vector<std::thread> ts;
    for (int p = 0; p < kProducers; ++p)
      ts.emplace_back([&, p] {
        std::mt19937 rng(static_cast<unsigned>(rep * 131 + p + 1));
        const std::uint64_t tag = std::uint64_t(p + 1) << 40;
        std::uint64_t seq = 1;
        while (seq <= static_cast<std::uint64_t>(n)) {
          if (rng() % 3 == 0) {
            wtc_c_queue_push_wait(&q, as_ptr(tag | seq++));
            continue;
          }
          own r(2 + rng() % 32);
          while (!r.full() && seq <= static_cast<std::uint64_t>(n) && rng() % 8) r.insert(as_ptr(tag | seq++));
          if (r.empty()) r.insert(as_ptr(tag | seq++));
          std::uint64_t first = as_int(r.data[r.tail]) & ((std::uint64_t{1} << 40) - 1);
          while (!r.empty()) {
            int limit = static_cast<int>(rng() % 14) - 1;
            const unsigned long before = r.count();
            const int k = push_batch(q, r, limit);
            EXPECT(k >= 0 && (limit > 0 ? k <= limit : k == 0));
            EXPECT(static_cast<unsigned long>(k) + r.count() == before);
            if (limit > 0) EXPECT(k > 0);
            if (k > 1) pushed[p].push_back({first, k});
            first += static_cast<std::uint64_t>(k);
          }
        }
        wtc_c_queue_terminate(&q);
      });
    for (int c = 0; c < kConsumers; ++c)
      ts.emplace_back([&, c] {
        std::mt19937 rng(static_cast<unsigned>(rep * 977 + c + 101));
        for (;;) {
          own d(2 + rng() % 32);
          // Some of the ring already used: the room is random too.
          const unsigned long used = rng() % d.size;
          d.tail = static_cast<int>(rng() % d.size);
          d.head = static_cast<int>((d.tail + used) % d.size);
          const unsigned long room = d.size - 1 - used;
          const int limit = static_cast<int>(rng() % 14) - 1;
          const int start = d.head;
          const int k = pop_batch(q, d, limit);
          if (k < 0) break;
          const unsigned long want = limit <= 0 ? 0 : std::min<unsigned long>(room, static_cast<unsigned long>(limit));
          EXPECT(static_cast<unsigned long>(k) <= want);
          if (want > 0) EXPECT(k > 0);
          EXPECT(d.count() == used + static_cast<unsigned long>(k));
          if (k == 0) continue;
          popped b{c, {}};
          for (int j = 0; j < k; ++j) b.elems.push_back(as_int(d.data[(start + j) % d.size]));
          got[c].push_back(std::move(b));
        }
      });
    for (auto& t : ts) t.join();
    wtc_c_queue_destroy(&q);
    // The batches of all consumers, each consumer's in turn (the graph
    // needs no other order of them).
    std::vector<popped> all;
    for (auto& g : got)
      for (auto& b : g) {
        if (b.elems.size() > 1) ++multi;
        all.push_back(std::move(b));
      }
    batches_total += static_cast<long>(all.size());
    std::string why;
    if (!ordered(all, pushed, kProducers, n, why)) {
      std::fprintf(stderr, "consumers: repetition %ld, capacity %ld: %s\n", rep, cap, why.c_str());
      ++g_failures;
    }
  }
  std::printf("consumers: %ld repetitions, %ld batches popped, %ld of more than one: %s\n", reps, batches_total, multi,
              g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// close: three producers; two consumers wait on an empty queue, one with
// the single pop and one with the batched pop. They share one element, and
// neither returns "finished" before the third termination.

void close_test() {
  object q;
  wtc_c_queue_init(&q, static_cast<int>(topology::generic), 4, 3, kExactly);
  std::atomic<int> finished{0}, taken{0};
  std::vector<std::thread> cs;
  for (int c = 0; c < 2; ++c)
    cs.emplace_back([&, c] {
      if (c == 0) {
        void* out;
        while (wtc_c_queue_pop_wait(&q, &out) == 1) {
          EXPECT(as_int(out) == 5);
          ++taken;
        }
      } else {
        own d(3);
        while (pop_batch(q, d, 2) >= 0)
          while (!d.empty()) {
            EXPECT(as_int(d.remove()) == 5);
            ++taken;
          }
      }
      ++finished;
    });
  wtc_c_queue_terminate(&q);
  wtc_c_queue_terminate(&q);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT(finished.load() == 0);
  wtc_c_queue_push_wait(&q, as_ptr(5));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT(finished.load() == 0);
  wtc_c_queue_terminate(&q);
  for (auto& t : cs) t.join();
  EXPECT(finished.load() == 2);
  EXPECT(taken.load() == 1);
  wtc_c_queue_destroy(&q);
  std::printf("close: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// crossing: the batch whose turn it is to be held stops in
// wtc_test_batch_step until the other side's batch has returned.

std::atomic<int> g_hold_side{-1};
std::atomic<bool> g_held{false}, g_go{false};

}  // namespace

extern "C" void wtc_test_batch_step(int side) {
  int want = side;
  if (!g_hold_side.compare_exchange_strong(want, -1)) return;
  g_held = true;
  while (!g_go) std::this_thread::yield();
}

namespace {

void crossing() {
  for (topology tag : {topology::spsc, topology::generic})
    for (int held : {1, 0}) {
      object q;
      wtc_c_queue_init(&q, static_cast<int>(tag), 4, 0, kNever);
      // The two queued by the thread that pushes the batch: with one
      // producer, WTC_CHECK holds the test to one.
      auto queue_two = [&] {
        wtc_c_queue_push_wait(&q, as_ptr(1));
        wtc_c_queue_push_wait(&q, as_ptr(2));
      };
      if (held == 1) queue_two();
      own src(16), dst(16);
      for (int i = 0; i < 10; ++i) src.insert(as_ptr(100 + i));
      int popped = -9, pushed = -9;
      g_held = false;
      g_go = false;
      g_hold_side = held;
      std::thread t([&] {
        if (held == 1) popped = pop_batch(q, dst, 10);
        else {
          queue_two();
          pushed = push_batch(q, src, 10);
        }
      });
      while (!g_held) std::this_thread::yield();
      if (held == 1) pushed = push_batch(q, src, 10);
      else popped = pop_batch(q, dst, 10);
      g_go = true;
      t.join();
      if (!((popped == 2 && pushed == 4) || (popped == 4 && pushed == 2))) {
        std::fprintf(stderr, "crossing, tag %d, %s held: the pop took %d and the push appended %d\n",
                     static_cast<int>(tag), held ? "pop" : "push", popped, pushed);
        ++g_failures;
      }
      wtc_c_queue_destroy(&q);
    }
  std::printf("crossing: %s\n", g_failures ? "FAILED" : "ok");
}

void two_consumers() {
  object q;
  wtc_c_queue_init(&q, static_cast<int>(topology::mpsc), 1024, 0, kNever);
  for (int i = 1; i <= 200; ++i) wtc_c_queue_push_wait(&q, as_ptr(i));
  auto take = [&] {
    own d(3);
    for (int i = 0; i < 50; ++i) {
      pop_batch(q, d, 2);
      d.head = d.tail;
    }
  };
  std::thread a(take), b(take);
  a.join();
  b.join();
}

void over_terminate() {
  object q;
  wtc_c_queue_init(&q, static_cast<int>(topology::generic), 4, 1, kExactly);
  wtc_c_queue_terminate(&q);
  wtc_c_queue_terminate(&q);
}

// ---------------------------------------------------------------------------
// atleast: a queue whose program tests `ended >= producers` takes extra
// terminations as the original does, staying finished.

void at_least() {
  object q;
  wtc_c_queue_init(&q, static_cast<int>(topology::generic), 4, 1, kAtLeast);
  wtc_c_queue_push_wait(&q, as_ptr(3));
  wtc_c_queue_terminate(&q);
  wtc_c_queue_terminate(&q);
  void* out = nullptr;
  EXPECT(wtc_c_queue_pop_wait(&q, &out) == 1 && as_int(out) == 3);
  EXPECT(wtc_c_queue_pop_wait(&q, &out) == 0);
  wtc_c_queue_terminate(&q);
  EXPECT(wtc_c_queue_pop_wait(&q, &out) == 0);
  wtc_c_queue_destroy(&q);
  std::printf("atleast: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// limit: a limit beyond any int, a size_t of -1 in the program, is no
// limit: the batch moves all it can.

void limit_test() {
  object q;
  wtc_c_queue_init(&q, static_cast<int>(topology::spsc), 8, 0, kNever);
  own src(4), dst(8);
  for (int i = 1; i <= 3; ++i) src.insert(as_ptr(i));
  EXPECT(wtc_c_queue_push_batch(&q, &src.data, &src.head, &src.tail, src.size, ~0ul) == 3);
  EXPECT(wtc_c_queue_pop_batch(&q, &dst.data, &dst.head, &dst.tail, dst.size, 1ul << 40) == 3);
  for (int i = 1; i <= 3; ++i) EXPECT(as_int(dst.remove()) == static_cast<std::uint64_t>(i));
  wtc_c_queue_destroy(&q);
  std::printf("limit: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// absorb: a batched push with nothing to move and a push with an element
// both asleep on a full queue; one pop makes room and wakes one of them.
// Whichever wakes, the push with an element must get through: a wake-up the
// empty batch takes is passed on. And the same on the other side: a batched
// pop with no room of its own and a pop asleep on an empty queue, one push.

void absorb() {
  {
    object q;
    wtc_c_queue_init(&q, static_cast<int>(topology::generic), 1, 0, kNever);
    wtc_c_queue_push_wait(&q, as_ptr(1));  // full
    own empty(4);
    std::atomic<bool> a_done{false}, b_done{false};
    std::thread a([&] {
      push_batch(q, empty, 8);
      a_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // asleep first: woken first
    std::thread b([&] {
      wtc_c_queue_push_wait(&q, as_ptr(2));
      b_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    void* out;
    EXPECT(wtc_c_queue_pop_wait(&q, &out) == 1);
    for (int i = 0; i < 200 && !b_done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!b_done) {
      std::fprintf(stderr, "absorb: the push with an element is still asleep with room in the queue\n");
      std::fflush(stderr);
      std::_Exit(1);
    }
    a.join();
    b.join();
    wtc_c_queue_destroy(&q);
  }
  {
    object q;
    wtc_c_queue_init(&q, static_cast<int>(topology::generic), 4, 0, kNever);
    own full(2);
    full.insert(as_ptr(9));  // no room
    std::atomic<bool> a_done{false}, b_done{false};
    std::thread a([&] {
      pop_batch(q, full, 8);
      a_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::thread b([&] {
      void* out;
      EXPECT(wtc_c_queue_pop_wait(&q, &out) == 1);
      b_done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    wtc_c_queue_push_wait(&q, as_ptr(1));
    for (int i = 0; i < 200 && !b_done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!b_done) {
      std::fprintf(stderr, "absorb: the pop is still asleep with an element in the queue\n");
      std::fflush(stderr);
      std::_Exit(1);
    }
    a.join();
    b.join();
    wtc_c_queue_destroy(&q);
  }
  std::printf("absorb: %s\n", g_failures ? "FAILED" : "ok");
}

// ---------------------------------------------------------------------------
// destroy (built with WTC_C_QUEUE_TEST_HOOK): a consumer that sees the
// queue finished destroys it at once, before joining the producer, as the
// original allows; the last termination is held in the runtime after it
// has finished the queue. destroy must wait for it.

std::atomic<bool> g_term_held{false}, g_destroyed{false}, g_early{false};

}  // namespace

extern "C" void wtc_c_queue_test_hook(const char* where) {
  if (std::string(where) != "terminate") return;
  g_term_held = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  if (g_destroyed) g_early = true;
}

namespace {

void destroy_after_finish() {
#ifndef WTC_C_QUEUE_TEST_HOOK
  std::fprintf(stderr, "destroy: needs WTC_C_QUEUE_TEST_HOOK\n");
  ++g_failures;
  return;
#endif
  for (int round = 0; round < 3; ++round) {
    object* q = new object;
    wtc_c_queue_init(q, static_cast<int>(topology::spsc), 4, 1, kExactly);
    g_term_held = false;
    g_destroyed = false;
    std::thread prod([&] {
      wtc_c_queue_push_wait(q, as_ptr(7));
      wtc_c_queue_terminate(q);
    });
    void* out;
    EXPECT(wtc_c_queue_pop_wait(q, &out) == 1);
    while (!g_term_held) std::this_thread::yield();
    EXPECT(wtc_c_queue_pop_wait(q, &out) == 0);  // finished
    wtc_c_queue_destroy(q);
    g_destroyed = true;
    delete q;
    prod.join();
    if (g_early) {
      std::fprintf(stderr, "destroy: returned while the last termination was still in the runtime\n");
      ++g_failures;
      break;
    }
  }
  std::printf("destroy: %s\n", g_failures ? "FAILED" : "ok");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: c_queue_test model|stress|consumers|close|crossing|twoconsumers|overterminate|atleast|limit|"
                 "absorb|destroy [N]\n");
    return 2;
  }
  const std::string mode = argv[1];
  const long n = argc > 2 ? std::atol(argv[2]) : 0;
  if (mode == "model") model(n ? n : 20000);
  else if (mode == "stress") stress(n ? n : 20000);
  else if (mode == "consumers") consumers_test(n ? n : 20000, argc > 3 ? std::atol(argv[3]) : 20);
  else if (mode == "close") close_test();
  else if (mode == "crossing") crossing();
  else if (mode == "twoconsumers") two_consumers();
  else if (mode == "overterminate") over_terminate();
  else if (mode == "atleast") at_least();
  else if (mode == "limit") limit_test();
  else if (mode == "absorb") absorb();
  else if (mode == "destroy") destroy_after_finish();
  else {
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
  }
  return g_failures ? 1 : 0;
}
