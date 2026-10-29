// Unit tests for wtc::rt::laned_mpsc, each on a situation a stress run
// reaches only by chance: the order within one producer, one producer's
// run made visible at once, a lane that is not starved by a full one, the
// lane of an exited producer drained before it is freed, a queue destroyed
// while a producer that pushed to it lives on, and the empty answer right
// after a new producer's first push.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include "laned_mpsc.hpp"

#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);       \
      std::exit(1);                                                           \
    }                                                                         \
  } while (0)

namespace {

// Counts the elements alive, so that a lane freed with elements in it, or
// an element destroyed twice, shows.
struct counted {
  static std::atomic<long> live;
  long v = 0;
  counted() { ++live; }
  explicit counted(long x) : v(x) { ++live; }
  counted(const counted& o) : v(o.v) { ++live; }
  counted(counted&& o) noexcept : v(o.v) { ++live; }
  counted& operator=(const counted& o) { v = o.v; return *this; }
  counted& operator=(counted&& o) noexcept { v = o.v; return *this; }
  ~counted() { --live; }
};
std::atomic<long> counted::live{0};

// One thread, which is producer and consumer: FIFO, the empty answer, and
// both ways to pop.
void one_thread() {
  wtc::rt::laned_mpsc<int> q;
  int x = -1;
  CHECK(!q.try_pop(x));
  for (int i = 0; i < 100000; ++i) q.push(i);
  for (int i = 0; i < 100000; ++i) {
    if (i % 2) {
      CHECK(q.try_pop(x));
    } else {
      CHECK(q.consume([&](int&& v) noexcept { x = v; }));
    }
    CHECK(x == i);
  }
  CHECK(!q.try_pop(x));
  CHECK(q.lanes() == 1);
}

// A run of deferred pushes is invisible until publish, then comes out
// whole and in order.
void deferred() {
  wtc::rt::laned_mpsc<int> q;
  int x = -1;
  for (int i = 0; i < 5; ++i) q.push_deferred(i);
  CHECK(!q.try_pop(x));
  q.publish();
  for (int i = 0; i < 5; ++i) {
    CHECK(q.try_pop(x));
    CHECK(x == i);
  }
  CHECK(!q.try_pop(x));
}

// One thread pushing to two queues keeps a lane in each, and the cached
// lookup follows the queue.
void two_queues() {
  wtc::rt::laned_mpsc<int> a, b;
  std::thread t([&] {
    for (int i = 0; i < 1000; ++i) {
      a.push(i);
      b.push(-i);
    }
  });
  t.join();
  int x;
  for (int i = 0; i < 1000; ++i) {
    CHECK(a.try_pop(x) && x == i);
    CHECK(b.try_pop(x) && x == -i);
  }
  CHECK(!a.try_pop(x) && !b.try_pop(x));
}

// A lane full of one producer's elements does not starve another's: with
// bursts of eight, the second producer's one element comes out within the
// first nine pops. Both producers have exited by then, and their lanes
// are drained to the end before they are freed.
void fair_and_drained() {
  {
    constexpr unsigned kBurst = 8;
    wtc::rt::laned_mpsc<counted, kBurst> q;
    std::thread a([&] { for (long i = 0; i < 10000; ++i) q.push(counted(i)); });
    a.join();
    std::thread b([&] { q.push(counted(1'000'000)); });
    b.join();
    counted x;
    long next = 0, pops = 0, b_at = -1;
    while (q.try_pop(x)) {
      ++pops;
      if (x.v == 1'000'000) {
        CHECK(b_at < 0);
        b_at = pops;
      } else {
        CHECK(x.v == next);
        ++next;
      }
    }
    CHECK(next == 10000);
    CHECK(b_at >= 1 && b_at <= static_cast<long>(kBurst) + 1);
    // Both producers are gone and their lanes are empty: the sweeps free
    // them.
    for (int i = 0; i < 1000; ++i) CHECK(!q.try_pop(x));
    CHECK(q.lanes() == 0);
    CHECK(q.retired() == 2);
  }
  CHECK(counted::live.load() == 0);
}

// The queue is destroyed while a thread that pushed to it lives on and
// then pushes to another queue: the thread's table must not touch the
// first queue's lane, and the elements left in it are destroyed with it.
void queue_dies_first() {
  {
    auto* q1 = new wtc::rt::laned_mpsc<counted>;
    wtc::rt::laned_mpsc<counted> q2;
    std::atomic<int> step{0};
    std::thread t([&] {
      for (long i = 0; i < 100; ++i) q1->push(counted(i));
      step.store(1, std::memory_order_release);
      while (step.load(std::memory_order_acquire) != 2) std::this_thread::yield();
      for (long i = 0; i < 100; ++i) q2.push(counted(i));
    });
    while (step.load(std::memory_order_acquire) != 1) std::this_thread::yield();
    counted x;
    for (long i = 0; i < 10; ++i) CHECK(q1->try_pop(x) && x.v == i);
    delete q1;  // ninety elements still in the lane
    step.store(2, std::memory_order_release);
    t.join();
    for (long i = 0; i < 100; ++i) CHECK(q2.try_pop(x) && x.v == i);
    CHECK(!q2.try_pop(x));
  }
  CHECK(counted::live.load() == 0);
}

// A new producer's push that has finished is seen by the next pop: its
// lane was registered before it pushed. Each round is a new thread, and
// the exited ones' lanes are freed as the rounds go.
void new_producer_seen() {
  wtc::rt::laned_mpsc<int> q;
  for (int round = 0; round < 2000; ++round) {
    std::atomic<bool> done{false};
    std::thread t([&] {
      q.push(round);
      done.store(true, std::memory_order_release);
    });
    while (!done.load(std::memory_order_acquire)) std::this_thread::yield();
    int x = -1;
    CHECK(q.try_pop(x));
    CHECK(x == round);
    t.join();
  }
  int x;
  for (int i = 0; i < 1000; ++i) CHECK(!q.try_pop(x));
  CHECK(q.lanes() == 0);
  CHECK(q.retired() == 2000);
}

}  // namespace

int main() {
  one_thread();
  deferred();
  two_queues();
  fair_and_drained();
  queue_dies_first();
  new_producer_seen();
  std::puts("laned_mpsc_test: ok");
  return 0;
}
