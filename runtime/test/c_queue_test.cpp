// Tests of the runtime of lifted C queues (shim/c_queue.h, c_queue.cpp),
// through its C entry points, as a lifted program calls them.
//
//   c_queue_test model [N]   random operations, one thread, against the
//                            original: a ring of `slots` pointers, and the
//                            caller's own rings; every answer, the queue's
//                            contents and the caller's rings' indices
//   c_queue_test stress [N]  producers that push single elements and
//                            batches from rings of their own, one consumer
//                            that pops batches, at capacities 1 to 20: every
//                            element arrives once, in order per producer,
//                            and every batch pushed arrives as adjacent
//                            elements
//   c_queue_test crossing    (built with WTC_TEST_BATCH_STEPS) a batched
//                            pop held between deciding its size and
//                            claiming its tickets while a batched push runs
//                            whole, and the other way round: two elements
//                            queued, capacity four, both asking for ten;
//                            the sizes must be an outcome of the original's
//                            mutex, two and four or four and two
//   c_queue_test twoconsumers  two consumers on a queue built for one, which
//                            the contract check must catch

#include "../../shim/c_queue.h"
#include "../../shim/topology.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>
#include <string>
#include <thread>
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

int push_batch(object& q, own& r, int limit) {
  return wtc_c_queue_push_batch(&q, &r.data, &r.head, &r.tail, r.size, limit);
}
int pop_batch(object& q, own& r, int limit) {
  return wtc_c_queue_pop_batch(&q, &r.data, &r.head, &r.tail, r.size, limit);
}

void* as_ptr(std::uint64_t v) { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(v)); }
std::uint64_t as_int(void* p) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)); }

// ---------------------------------------------------------------------------
// model: the original's answers, operation by operation, where they do not
// wait.

void model(long n) {
  std::mt19937_64 rng(7);
  for (topology tag : {topology::spsc, topology::mpsc, topology::spmc, topology::generic})
    for (long cap = 0; cap <= 6; ++cap)
      for (int round = 0; round < 3; ++round) {
        object q;
        wtc_c_queue_init(&q, static_cast<int>(tag), cap);
        std::deque<std::uint64_t> m;
        std::uint64_t next = 1;
        own src(1 + rng() % 6), dst(1 + rng() % 6);
        std::deque<std::uint64_t> srcM, dstM;
        for (long i = 0; i < n; ++i) {
          const bool full = static_cast<long>(m.size()) >= cap;
          switch (rng() % 6) {
          case 0:
            if (!full) {
              wtc_c_queue_push_wait(&q, as_ptr(next));
              m.push_back(next++);
            }
            break;
          case 1:
            if (!m.empty()) {
              void* out = nullptr;
              EXPECT(wtc_c_queue_pop_wait(&q, &out) == 1);
              EXPECT(as_int(out) == m.front());
              m.pop_front();
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
            if (!m.empty()) {
              const int limit = static_cast<int>(rng() % 5) - 1;
              const int r = pop_batch(q, dst, limit);
              int want = 0;
              while (limit > 0 && want < limit && want < static_cast<int>(m.size()) &&
                     dstM.size() + want < dst.size - 1)
                ++want;
              EXPECT(r == want);
              for (int j = 0; j < r && j < want; ++j) {
                dstM.push_back(m.front());
                m.pop_front();
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
      wtc_c_queue_init(&q, static_cast<int>(tag), cap);
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
        });
      std::vector<std::uint64_t> seen;
      own d(21);
      const std::size_t total = static_cast<std::size_t>(producers) * static_cast<std::size_t>(n);
      while (seen.size() < total) {
        pop_batch(q, d, 20);
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
      wtc_c_queue_init(&q, static_cast<int>(tag), 4);
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
  wtc_c_queue_init(&q, static_cast<int>(topology::mpsc), 1024);
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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: c_queue_test model|stress|crossing|twoconsumers [N]\n");
    return 2;
  }
  const std::string mode = argv[1];
  const long n = argc > 2 ? std::atol(argv[2]) : 0;
  if (mode == "model") model(n ? n : 20000);
  else if (mode == "stress") stress(n ? n : 20000);
  else if (mode == "crossing") crossing();
  else if (mode == "twoconsumers") two_consumers();
  else {
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
  }
  return g_failures ? 1 : 0;
}
