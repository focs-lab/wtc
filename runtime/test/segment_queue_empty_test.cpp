// The empty answer and the out-of-order take of wtc::rt::segment_queue,
// deterministically, through the queue's test hooks. A push is held
// between taking its cell and filling it, a later push finishes, and the
// queue is popped. A linearisable queue may not answer empty there: the
// later push finished before the pop began, so its element is in the queue
// whatever order the held push takes. The multi-consumer form waits for
// the held cell; the single-consumer form takes the later element. Also:
// the single consumer's second look finding an earlier cell, and finding
// the head cell; a long run behind a held cell costing a bounded number of
// looks per pop; every element destroyed exactly once.

#include "segment_queue.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

// A push made by a thread whose gate is set is held after taking its cell.
struct gate {
  std::atomic<bool> held{false};
  std::atomic<bool> release{false};
  std::atomic<bool> done{false};
};
thread_local gate* g_mine = nullptr;
void hold_here() {
  gate* g = g_mine;
  if (!g) return;
  g_mine = nullptr;
  g->held.store(true, std::memory_order_release);
  while (!g->release.load(std::memory_order_acquire)) std::this_thread::yield();
}

std::atomic<long> g_live{0};
struct counted {
  int v = 0;
  counted() { g_live.fetch_add(1, std::memory_order_relaxed); }
  explicit counted(int x) : v(x) { g_live.fetch_add(1, std::memory_order_relaxed); }
  counted(const counted& o) : v(o.v) { g_live.fetch_add(1, std::memory_order_relaxed); }
  counted(counted&& o) noexcept : v(o.v) { g_live.fetch_add(1, std::memory_order_relaxed); }
  counted& operator=(const counted& o) { v = o.v; return *this; }
  counted& operator=(counted&& o) noexcept { v = o.v; return *this; }
  ~counted() { g_live.fetch_sub(1, std::memory_order_relaxed); }
};

// The hooks the queues here are built with: each calls the function the
// test has set, if any, and the single consumer's looks are counted.
struct hooks : wtc::rt::no_hooks {
  static inline void (*on_reserved)() = nullptr;
  static inline void (*on_between_looks)() = nullptr;
  static inline std::uint64_t looks = 0;
  static void reserved() { if (on_reserved) on_reserved(); }
  static void between_looks() { if (on_between_looks) on_between_looks(); }
  static void look() { ++looks; }
};
template <bool kMany, unsigned kSegment>
using queue = wtc::rt::segment_queue<counted, kMany, kSegment, 64, hooks>;

int failures = 0;
#define EXPECT(c)                                                             \
  do {                                                                        \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c);  \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

// Starts a thread whose push of v is held after taking its cell.
template <class Q>
std::thread held_push(Q& q, gate& g, int v) {
  std::thread t([&q, &g, v] {
    g_mine = &g;
    q.push(counted(v));
    g.done.store(true, std::memory_order_release);
  });
  while (!g.held.load(std::memory_order_acquire)) std::this_thread::yield();
  return t;
}
void finish(gate& g, std::thread& t) {
  g.release.store(true, std::memory_order_release);
  t.join();
}

// Single consumer: the later elements come out while the held push is held,
// across segments (four cells each here), and the held one after it is
// released.
void one_consumer() {
  {
    queue<false, 4> q;
    gate a;
    std::thread ta = held_push(q, a, 1);
    for (int v = 2; v <= 9; ++v) q.push(counted(v));
    counted e;
    for (int v = 2; v <= 9; ++v) {
      bool got = q.try_pop(e);
      EXPECT(got);
      if (got) EXPECT(e.v == v);
    }
    // Only the held push is left, and it has not finished: empty is right.
    EXPECT(!q.try_pop(e));
    finish(a, ta);
    EXPECT(q.try_pop(e) && e.v == 1);
    EXPECT(!q.try_pop(e));
    q.push(counted(10));
    EXPECT(q.try_pop(e) && e.v == 10);
    EXPECT(!q.try_pop(e));
  }
  EXPECT(g_live.load() == 0);
  {
    // Destroyed with an element taken out of order and two still in it.
    queue<false, 4> q;
    gate a;
    std::thread ta = held_push(q, a, 1);
    q.push(counted(2));
    q.push(counted(3));
    counted e;
    EXPECT(q.try_pop(e) && e.v == 2);
    finish(a, ta);
  }
  EXPECT(g_live.load() == 0);
}

// The second look finds an earlier cell: cells 0 and 1 are held, cell 2 is
// full; between the looks cell 1's push finishes, so the pop must return
// it, not cell 2 (its push finished before the pop's second look began).
gate* g_release_at_look = nullptr;
void release_at_look() {
  gate* g = g_release_at_look;
  if (!g) return;
  g_release_at_look = nullptr;
  g->release.store(true, std::memory_order_release);
  while (!g->done.load(std::memory_order_acquire)) std::this_thread::yield();
}
void second_look() {
  hooks::on_between_looks = release_at_look;
  {
    queue<false, 8> q;
    gate a, b;
    std::thread ta = held_push(q, a, 1);
    std::thread tb = held_push(q, b, 2);
    q.push(counted(3));
    g_release_at_look = &b;
    counted e;
    EXPECT(q.try_pop(e) && e.v == 2);
    tb.join();
    EXPECT(q.try_pop(e) && e.v == 3);
    EXPECT(!q.try_pop(e));
    finish(a, ta);
    EXPECT(q.try_pop(e) && e.v == 1);
    EXPECT(!q.try_pop(e));
  }
  EXPECT(g_live.load() == 0);
  {
    // The cell the second look finds is the head cell itself.
    queue<false, 8> q;
    gate a;
    std::thread ta = held_push(q, a, 1);
    q.push(counted(2));
    g_release_at_look = &a;
    counted e;
    EXPECT(q.try_pop(e) && e.v == 1);
    ta.join();
    EXPECT(q.try_pop(e) && e.v == 2);
    EXPECT(!q.try_pop(e));
    q.push(counted(3));
    EXPECT(q.try_pop(e) && e.v == 3);
  }
  EXPECT(g_live.load() == 0);
  hooks::on_between_looks = nullptr;
}

// A hole filled after it was noted, then a push past where the walk
// stopped: the hole's element, whose push finished first, comes out first.
void filled_hole_first() {
  {
    queue<false, 8> q;
    gate a;
    std::thread ta = held_push(q, a, 1);  // cell 0, noted as a hole below
    q.push(counted(2));                    // cell 1
    counted e;
    EXPECT(q.try_pop(e) && e.v == 2);
    finish(a, ta);                         // the hole is filled now
    q.push(counted(3));                    // cell 2, past the walk
    EXPECT(q.try_pop(e) && e.v == 1);
    EXPECT(q.try_pop(e) && e.v == 3);
    EXPECT(!q.try_pop(e));
  }
  EXPECT(g_live.load() == 0);
}

// Holes in a later segment while the head's segment is retired (four
// cells a segment): cell 2 and cell 6 are held, the rest are full.
void holes_across_segments() {
  {
    queue<false, 4> q;
    q.push(counted(0));
    q.push(counted(1));
    gate a, b;
    std::thread ta = held_push(q, a, 2);
    q.push(counted(3));
    q.push(counted(4));
    q.push(counted(5));
    std::thread tb = held_push(q, b, 6);
    q.push(counted(7));
    counted e;
    for (int v : {0, 1, 3, 4, 5, 7}) EXPECT(q.try_pop(e) && e.v == v);
    EXPECT(!q.try_pop(e));
    finish(a, ta);
    EXPECT(q.try_pop(e) && e.v == 2);
    EXPECT(!q.try_pop(e));  // steps past the first segment and retires it
    finish(b, tb);
    EXPECT(q.try_pop(e) && e.v == 6);
    EXPECT(!q.try_pop(e));
    q.push(counted(8));
    EXPECT(q.try_pop(e) && e.v == 8);
  }
  EXPECT(g_live.load() == 0);
}

// A long run behind a held cell: each pop looks at a bounded number of
// cells, so the run costs O(k), not O(k^2) as a walk from the head would.
void long_run() {
  constexpr int k = 100000;
  {
    queue<false, 64> q;
    gate a;
    std::thread ta = held_push(q, a, 0);
    for (int v = 1; v <= k; ++v) q.push(counted(v));
    hooks::looks = 0;
    auto t0 = std::chrono::steady_clock::now();
    counted e;
    int got = 0;
    for (int v = 1; v <= k; ++v) got += q.try_pop(e) && e.v == v;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT(got == k);
    EXPECT(hooks::looks <= 4ull * k);
    std::printf("long_run: %d pops behind a held cell, %llu looks, %lld ms\n", k,
                static_cast<unsigned long long>(hooks::looks), static_cast<long long>(ms));
    finish(a, ta);
    EXPECT(q.try_pop(e) && e.v == 0);
    EXPECT(!q.try_pop(e));
  }
  EXPECT(g_live.load() == 0);
}

// Many consumers: the pop waits for the held cell instead of answering
// empty, and returns the held element once it is filled.
void many_consumers() {
  {
    queue<true, 4> q;
    gate a;
    std::thread ta = held_push(q, a, 1);
    q.push(counted(2));
    std::atomic<int> result{-1};
    std::thread c([&] {
      counted e;
      result.store(q.try_pop(e) ? e.v : 0, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT(result.load(std::memory_order_acquire) == -1);  // still waiting, not "empty"
    finish(a, ta);
    c.join();
    EXPECT(result.load() == 1);
    counted e;
    EXPECT(q.try_pop(e) && e.v == 2);
    EXPECT(!q.try_pop(e));
  }
  EXPECT(g_live.load() == 0);
}

}  // namespace

int main() {
  hooks::on_reserved = hold_here;
  one_consumer();
  second_look();
  filled_hole_first();
  holes_across_segments();
  long_run();
  many_consumers();
  if (failures) {
    std::fprintf(stderr, "segment_queue_empty_test: %d failures\n", failures);
    return 1;
  }
  std::printf("segment_queue_empty_test: no empty answer while a finished push's element is queued\n");
  return 0;
}
