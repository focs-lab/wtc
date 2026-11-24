// The hazard-pointer domain (hazard.hpp) under thread churn.
//
//   churn N   N threads, kWave at a time, each pushing and popping on a
//             general-purpose queue of small segments and pushing to a
//             single-consumer queue that the main thread drains: every
//             element arrives once, and the records are reused, so that no
//             more are ever made than threads held them at once.
//   late      threads whose last round of key destructors (glibc runs at
//             most four) pushes to and pops from queues, after the
//             hazard key's destructor gave their records back: those
//             operations borrow a record each and give it back, so fifty
//             such threads, one at a time, leave no record taken.
//   late_two  one such thread makes two operations in its last round, and
//             between them another thread takes a record and holds it:
//             the second operation borrows a record of its own, never the
//             one the first gave back and the other thread now holds.
//   fork      a fork while eight threads hold records: in the child, eight
//             new threads holding records at once take the records the
//             parent's threads held, and no more are made.
//   tail      the single consumer retires a segment while the tail still
//             names it (no push has gone past it yet): it must move the
//             tail on first, or the next push protects the freed segment.
//             Built as hazard_eager_test, where a scan runs at every
//             retire (WTC_HAZARD_SCAN_AT=1) and freed memory is
//             overwritten, so that such a push crashes; with the usual
//             threshold the segment outlives the window.
//   overlap P S  in hazard_all_test (scans at every retire, every push
//             marking): P producers push for S seconds while the main
//             thread pops, the producers keeping the backlog under 4096
//             elements; the marks of pushes overlap at nearly every
//             scan, and the consumer's list of retired segments stays
//             within the queue's backlog, not its history: a scan keeps
//             only those retired since the oldest mark's count, which lie
//             below that mark's push's index. A producer preempted while
//             it holds its mark (more threads than processors) makes that
//             the whole backlog of the moment it took its index.
//   relay     in hazard_all_test: a push held with the queue's mark while
//             the consumer retires three segments, then another held with
//             its own while the consumer retires more: the scans under the
//             second free the segments retired before its count.
//   nested    a general-purpose push of a batch whose first element's move
//             pushes to another queue, and whose third is held in its move
//             on its way to a new segment, while another push links the
//             next segment and pops drain the first and retire it: the
//             nested push has its own slot, so the outer push's segment
//             stays protected; with one shared slot it was freed under the
//             outer push, which then linked its new segment through it:
//             built as hazard_zero_test, which scans at every retire and
//             zeroes freed memory, that link succeeds and the elements it
//             carries are lost.
//   deep      pushes nested in elements' moves, one inside another: four
//             inside an outer push are fine, the fifth aborts.
//   producers P N  P producers pushing N elements each, single and in
//             runs, to the single-consumer queue the main thread drains:
//             every element once, in its producer's order. Built as
//             hazard_all_test too, with no try at protecting the tail
//             (WTC_TEST_PROTECT_TRIES=0), so that every push marks its
//             queue's segments to be kept instead (hazard::protect_all),
//             scans at every retire, and freed memory overwritten.
//   pin       in hazard_all_test: a push of one queue held while it marks
//             that queue's segments, and another queue's thousands of
//             segments pushed through and retired meanwhile: they are
//             freed, as the mark is the first queue's alone.
//   stalled   a pop held after protecting a segment while another thread
//             moves the head on and retires fifty segments: the held
//             segment survives, and the retiring thread keeps at most
//             kSlots segments (the held reader's slots) after a scan.
#include <malloc.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "hazard.hpp"
#include "segment_queue.hpp"

#ifdef WTC_TEST_POISON_FREED
// Freed memory overwritten, so that a read of a freed segment shows, with
// 0xdb (pointers that fault) or with WTC_TEST_POISON_BYTE (0: a freed
// segment then looks like the last one, and a push that links through it
// loses what it links). The empty asm keeps the compiler from dropping the
// stores as dead.
#ifndef WTC_TEST_POISON_BYTE
#define WTC_TEST_POISON_BYTE 0xdb
#endif
void operator delete(void* p) noexcept {
  if (p) {
    std::memset(p, WTC_TEST_POISON_BYTE, malloc_usable_size(p));
    __asm__ __volatile__("" : : "r"(p) : "memory");
  }
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete(void* p, std::align_val_t) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { operator delete(p); }
#endif

// WTC_TEST_ONE_COUNTER: the single-consumer queues take the one-counter
// push (the shim's jiffy).
#ifndef WTC_TEST_ONE_COUNTER
#define WTC_TEST_ONE_COUNTER 0
#endif
constexpr bool kOne = WTC_TEST_ONE_COUNTER;

namespace {
int failures = 0;

#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

constexpr unsigned kWave = 16;

void churn(long n) {
  // Eight cells a segment: segments are retired all the time, by threads
  // that then exit with some of them still in their records.
  wtc::rt::segment_queue<std::uint64_t, true, 8> g;
  wtc::rt::segment_queue<std::uint64_t, false, 8, 64, wtc::rt::no_hooks, kOne> m;
  std::atomic<long> popped{0};
  std::uint64_t sum_in = 0, sum_out = 0;
  long got = 0;
  for (long base = 0; base < n; base += kWave) {
    std::vector<std::thread> ts;
    // The wave's threads hold their records at once: each waits at the
    // start for the others to have taken theirs.
    const unsigned wave = static_cast<unsigned>(n - base < kWave ? n - base : kWave);
    std::atomic<unsigned> arrived{0};
    for (unsigned k = 0; k < wave; ++k) {
      const std::uint64_t id = static_cast<std::uint64_t>(base + k);
      sum_in += id;
      ts.emplace_back([&, id, wave] {
        g.push(id);
        arrived.fetch_add(1);
        while (arrived.load() < wave) std::this_thread::yield();
        for (int i = 1; i < 5; ++i) g.push(id);
        std::uint64_t v;
        for (int i = 0; i < 5; ++i)
          if (g.try_pop(v)) popped.fetch_add(1);
        m.push(id);
      });
    }
    for (auto& t : ts) t.join();
    std::uint64_t v;
    while (m.try_pop(v)) {
      sum_out += v;
      ++got;
    }
  }
  std::uint64_t v;
  while (g.try_pop(v)) popped.fetch_add(1);
  EXPECT(got == n);
  EXPECT(sum_out == sum_in);
  EXPECT(popped.load() == 5 * n);
  // The waves' threads and the main thread.
  const unsigned made = wtc::rt::hazard::records_made();
  std::printf("churn: %ld threads, %u records made\n", n, made);
  EXPECT(made >= kWave);
  EXPECT(made <= kWave + 1);
}
// Hooks: a pop holds at stage 1, after taking its cell, while t_hold is set.
std::atomic<bool> g_at{false}, g_go{false};
thread_local bool t_hold = false;
struct hooks : wtc::rt::no_hooks {
  static void pop_stage(unsigned stage) {
    if (stage != 1 || !t_hold) return;
    t_hold = false;
    g_at.store(true);
    while (!g_go.load()) std::this_thread::yield();
  }
};

void stalled() {
  wtc::rt::segment_queue<std::uint64_t, true, 8, 64, hooks> q;
  for (std::uint64_t i = 0; i < 8; ++i) q.push(i);
  std::uint64_t v = 0, held = ~std::uint64_t{0};
  for (std::uint64_t i = 0; i < 7; ++i) EXPECT(q.try_pop(v) && v == i);
  std::thread reader([&] {
    t_hold = true;
    EXPECT(q.try_pop(held));  // takes cell 7, the first segment's last, and is held
  });
  while (!g_at.load()) std::this_thread::yield();
  for (std::uint64_t i = 0; i < 400; ++i) q.push(100 + i);
  for (std::uint64_t i = 0; i < 400; ++i) EXPECT(q.try_pop(v) && v == 100 + i);
  wtc::rt::hazard::record& me = wtc::rt::hazard::mine();
  wtc::rt::hazard::scan(me);
  std::printf("stalled: %zu segments kept by the retiring thread after a scan\n", me.retired_count);
  EXPECT(me.retired_count >= 1);  // the held one
  EXPECT(me.retired_count <= wtc::rt::hazard::kSlots);
  g_go.store(true);
  reader.join();
  EXPECT(held == 7);
  wtc::rt::hazard::scan(me);
  EXPECT(me.retired_count == 0);
}
// A key whose destructor sets itself again, round after round, and makes
// its operations in the round asked for.
pthread_key_t g_late_key;
long g_late_round = 4;
wtc::rt::segment_queue<std::uint64_t, true, 8>* g_late_g = nullptr;
wtc::rt::segment_queue<std::uint64_t, false, 8, 64, wtc::rt::no_hooks, kOne>* g_late_m = nullptr;
void late_dtor(void* p) {
  const long round = reinterpret_cast<long>(p);
  if (round == g_late_round) {
    g_late_g->push(std::uint64_t{1});
    std::uint64_t v;
    g_late_g->try_pop(v);
    g_late_m->push(std::uint64_t{2});
  }
  if (round < 4) pthread_setspecific(g_late_key, reinterpret_cast<void*>(round + 1));
}

void late() {
  wtc::rt::segment_queue<std::uint64_t, true, 8> g;
  wtc::rt::segment_queue<std::uint64_t, false, 8, 64, wtc::rt::no_hooks, kOne> m;
  g_late_g = &g;
  g_late_m = &m;
  EXPECT(pthread_key_create(&g_late_key, late_dtor) == 0);
  for (int t = 0; t < 50; ++t)
    std::thread([] {
      g_late_g->push(std::uint64_t{0});
      std::uint64_t v;
      g_late_g->try_pop(v);
      pthread_setspecific(g_late_key, reinterpret_cast<void*>(1));
    }).join();
  std::uint64_t v;
  long got = 0;
  while (m.try_pop(v)) ++got;
  const unsigned made = wtc::rt::hazard::records_made();
  std::printf("late: 50 threads operating in their last key-destructor round: %ld pushes arrived, "
              "%u records made\n", got, made);
  EXPECT(got == 50);
  // Each thread's own record and the borrowed one, reused by the next
  // thread, and the main thread's.
  EXPECT(made <= 3);
}

// late_two: the exiting thread's two operations and the thread between them.
pthread_key_t g_late2_key;
wtc::rt::segment_queue<std::uint64_t, true, 8>* g_late2_q = nullptr;
std::atomic<int> g_late2_stage{0};
std::atomic<wtc::rt::hazard::record*> g_late2_held{nullptr}, g_late2_second{nullptr};
void late2_dtor(void* p) {
  const long round = reinterpret_cast<long>(p);
  if (round == 4) {
    g_late2_q->push(std::uint64_t{1});  // the first: borrows a record and gives it back
    g_late2_stage = 1;
    while (g_late2_stage.load() != 2) std::this_thread::yield();
    const wtc::rt::hazard::use second;  // the second
    g_late2_second = &static_cast<wtc::rt::hazard::record&>(second);
    g_late2_q->push(std::uint64_t{2});
  }
  if (round < 4) pthread_setspecific(g_late2_key, reinterpret_cast<void*>(round + 1));
}

void late_two() {
  wtc::rt::segment_queue<std::uint64_t, true, 8> q;  // the main thread takes no record
  g_late2_q = &q;
  EXPECT(pthread_key_create(&g_late2_key, late2_dtor) == 0);
  std::thread holder([] {
    while (g_late2_stage.load() != 1) std::this_thread::yield();
    g_late2_q->push(std::uint64_t{3});  // takes the idle record, the one the first gave back
    g_late2_held = &wtc::rt::hazard::mine();
    g_late2_stage = 2;
    while (g_late2_stage.load() != 3) std::this_thread::yield();
  });
  std::thread([] {
    g_late2_q->push(std::uint64_t{0});
    pthread_setspecific(g_late2_key, reinterpret_cast<void*>(1));
  }).join();
  g_late2_stage = 3;
  holder.join();
  std::printf("late_two: the second late operation's record %p, the other thread's %p\n",
              static_cast<void*>(g_late2_second.load()), static_cast<void*>(g_late2_held.load()));
  EXPECT(g_late2_second.load() != nullptr);
  EXPECT(g_late2_second.load() != g_late2_held.load());
  std::uint64_t v;
  int got = 0;
  while (q.try_pop(v)) ++got;
  EXPECT(got == 4);
}

void forked() {
  wtc::rt::segment_queue<std::uint64_t, false, 1024, 64, wtc::rt::no_hooks, kOne> q;
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::vector<std::thread> holders;
  for (int t = 0; t < 8; ++t)
    holders.emplace_back([&] {
      q.push(1);
      ready.fetch_add(1);
      while (!stop.load()) std::this_thread::yield();
    });
  while (ready.load() < 8) std::this_thread::yield();
  const unsigned made = wtc::rt::hazard::records_made();
  std::fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    std::atomic<int> in{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
      ts.emplace_back([&] {
        q.push(2);
        in.fetch_add(1);
        while (in.load() < 8) std::this_thread::yield();
      });
    for (auto& t : ts) t.join();
    const unsigned now = wtc::rt::hazard::records_made();
    std::printf("fork: records %u before, %u after eight threads in the child\n", made, now);
    std::fflush(stdout);
    _exit(now == made ? 0 : 1);
  }
  int status = 0;
  EXPECT(waitpid(pid, &status, 0) == pid);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  stop = true;
  for (auto& t : holders) t.join();
}

void tail() {
  wtc::rt::segment_queue<std::uint64_t, false, 4, 64, wtc::rt::no_hooks, kOne> q;
  // Four cells in the first segment; the push of the second links the
  // next, and no push has gone past the first, so the tail stays there.
  for (std::uint64_t i = 0; i < 4; ++i) q.push(i);
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < 4; ++i) EXPECT(q.try_pop(v) && v == i);
  EXPECT(!q.try_pop(v));  // the head moves on to the second segment
  EXPECT(!q.try_pop(v));  // and the first is retired
  for (std::uint64_t i = 4; i < 40; ++i) q.push(i);
  for (std::uint64_t i = 4; i < 40; ++i) EXPECT(q.try_pop(v) && v == i);
  EXPECT(!q.try_pop(v));
  std::printf("tail: a segment retired while the tail named it, then 36 pushes and pops\n");
}

// The nested test's elements: 102's move pushes to another queue, 104's
// is held until the test lets it go.
std::atomic<bool> g_nest_armed{false}, g_nest_inside{false}, g_nest_go{false};
wtc::rt::segment_queue<int, true, 4>* g_nest_other = nullptr;
struct nest_elem {
  int v = 0;
  nest_elem() = default;
  explicit nest_elem(int x) : v(x) {}
  nest_elem(nest_elem&& o) noexcept : v(o.v) {
    if (!g_nest_armed.load()) return;
    if (v == 102) g_nest_other->push(1);
    if (v == 104) {
      g_nest_inside.store(true);
      while (!g_nest_go.load()) std::this_thread::yield();
    }
  }
  nest_elem& operator=(nest_elem&& o) noexcept {
    v = o.v;
    return *this;
  }
};

void nested() {
  wtc::rt::segment_queue<int, true, 4> other;
  g_nest_other = &other;
  wtc::rt::segment_queue<nest_elem, true, 4> q;
  q.push(nest_elem(0));
  q.push(nest_elem(1));
  std::thread p([&] {
    nest_elem run[4] = {nest_elem(102), nest_elem(103), nest_elem(104), nest_elem(105)};
    g_nest_armed.store(true);
    q.push_run(run, 4);  // cells 2 and 3, then a new segment for 104 and 105
  });
  while (!g_nest_inside.load()) std::this_thread::yield();
  g_nest_armed.store(false);
  q.push(nest_elem(7));  // links the next segment first
  nest_elem out;
  std::vector<int> got;
  for (int k = 0; k < 5; ++k)
    if (q.try_pop(out)) got.push_back(out.v);
  for (int i = 0; i < 16; ++i) {  // more retires and scans
    q.push(nest_elem(200 + i));
    if (q.try_pop(out)) got.push_back(out.v);
  }
  g_nest_go.store(true);
  p.join();
  while (q.try_pop(out)) got.push_back(out.v);
  int v = 0;
  EXPECT(other.try_pop(v) && v == 1);
  // Every element once: 0 1 102 103, 7, 200..215, 104 105.
  std::vector<int> want = {0, 1, 102, 103, 7, 104, 105};
  for (int i = 0; i < 16; ++i) want.push_back(200 + i);
  std::sort(got.begin(), got.end());
  std::sort(want.begin(), want.end());
  std::printf("nested: %zu elements popped, %s\n", got.size(), got == want ? "each once" : "NOT each once");
  EXPECT(got == want);
}

// A push in a move, in a move, ...: the fifth nested push has no slot.
wtc::rt::segment_queue<struct deep_elem, true, 4>* g_deep = nullptr;
struct deep_elem {
  int depth = 0;
  deep_elem() = default;
  explicit deep_elem(int d) : depth(d) {}
  // Only the move into a cell pushes: the element pushed is built in
  // place, not moved, on its way in.
  deep_elem(deep_elem&& o) noexcept : depth(o.depth) {
    if (depth == 0) return;
    deep_elem inner(depth - 1);
    g_deep->push_run(&inner, 1);
  }
  deep_elem& operator=(deep_elem&& o) noexcept {
    depth = o.depth;
    return *this;
  }
};

void deep() {
  wtc::rt::segment_queue<deep_elem, true, 4> q;
  g_deep = &q;
  deep_elem four(4);
  q.push_run(&four, 1);  // a push and four nested in it: fine
  std::printf("deep: four nested pushes done\n");
  std::fflush(stdout);
  deep_elem five(5);
  q.push_run(&five, 1);  // five nested: aborts
  std::printf("deep: five nested pushes done\n");
}

void overlap(unsigned p, int secs) {
  wtc::rt::segment_queue<int, false, 64, 64, wtc::rt::no_hooks, kOne> q;
  std::atomic<bool> stop{false};
  std::vector<std::thread> ps;
  // The producers keep the backlog under kBacklog elements, so that the
  // list the marks may keep is bounded while the queue's history grows.
  // pushed is counted before each push, so that pushed - pops is at least
  // the backlog.
  constexpr long kBacklog = 4096;
  std::atomic<long> pushed{0}, popped{0};
  for (unsigned t = 0; t < p; ++t)
    ps.emplace_back([&] {
      for (int k = 0; !stop.load(std::memory_order_relaxed); ++k) {
        while (pushed.load(std::memory_order_relaxed) - popped.load(std::memory_order_relaxed) > kBacklog &&
               !stop.load(std::memory_order_relaxed))
          std::this_thread::yield();
        pushed.fetch_add(1, std::memory_order_relaxed);
        q.push(k);
      }
    });
  wtc::rt::hazard::record& me = wtc::rt::hazard::mine();
  std::size_t most = 0;
  long pops = 0, backlog = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
    int v;
    if (q.try_pop(v)) popped.store(++pops, std::memory_order_relaxed);
    if (me.retired_count > most) most = me.retired_count;
    const long b = pushed.load(std::memory_order_relaxed) - pops;
    if (b > backlog) backlog = b;
  }
  stop = true;
  for (auto& t : ps) t.join();
  int v;
  while (q.try_pop(v)) ++pops;
  // The segments the largest backlog spanned, one more for its ends, and
  // the segments the records' slots name.
  const std::size_t bound = static_cast<std::size_t>(backlog / 64 + 2) + p + 1;
  std::printf("overlap: %u producers, %ld pops (%ld segments), at most %zu segments waiting to be freed, "
              "the backlog at most %ld elements (bound %zu)\n",
              p, pops, pops / 64, most, backlog, bound);
  EXPECT(most <= bound);
}

// A push of queue a held at its first step to a later segment, while it
// holds a's mark (in hazard_all_test every push does); queue b's segments
// are counted.
std::atomic<bool> g_pin_at{false}, g_pin_go{false};
thread_local bool t_pin = false;
std::atomic<long> g_b_bodies{0};
struct pin_hooks : wtc::rt::no_hooks {
  static void segment_step(int) {
    if (!t_pin) return;
    t_pin = false;
    g_pin_at.store(true);
    while (!g_pin_go.load()) std::this_thread::yield();
  }
};
struct count_hooks : wtc::rt::no_hooks {
  static void body(int d) { g_b_bodies.fetch_add(d); }
};

void pin() {
  wtc::rt::segment_queue<std::uint64_t, false, 4, 64, pin_hooks, kOne> a;
  wtc::rt::segment_queue<std::uint64_t, false, 4, 64, count_hooks, kOne> b;
  for (std::uint64_t i = 0; i < 4; ++i) a.push(i);  // the tail stays at the first segment
  std::thread p([&] {
    t_pin = true;
    a.push(std::uint64_t{4});  // walks on to the second segment, and is held there
  });
  while (!g_pin_at.load()) std::this_thread::yield();
  long most = 0;
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < 40000; ++i) {
    b.push(i);
    EXPECT(b.try_pop(v) && v == i);
    const long now = g_b_bodies.load();
    if (now > most) most = now;
  }
  std::printf("pin: a push of one queue held with its mark; the other's segments alive at most %ld\n", most);
  EXPECT(most <= 16);
  g_pin_go.store(true);
  p.join();
  for (std::uint64_t i = 0; i < 5; ++i) EXPECT(a.try_pop(v) && v == i);
}

// A push held with the queue's mark while the consumer retires segments,
// then another held with its own mark while the consumer retires more:
// the segments retired under the first mark only are freed by the scans
// under the second, which keep only what was retired after its count.
void relay() {
  wtc::rt::segment_queue<std::uint64_t, false, 4, 64, pin_hooks, kOne> q;
  auto held_push = [&q](std::uint64_t x) {
    g_pin_at = false;
    g_pin_go = false;
    std::thread t([&q, x] {
      t_pin = true;
      q.push(x);  // walks on to the next segment, and is held there with its mark
    });
    while (!g_pin_at.load()) std::this_thread::yield();
    return t;
  };
  wtc::rt::hazard::record& me = wtc::rt::hazard::mine();
  std::uint64_t v = 0;
  for (std::uint64_t i = 0; i < 16; ++i) q.push(i);
  std::thread a = held_push(16);
  for (std::uint64_t i = 0; i < 16; ++i) EXPECT(q.try_pop(v) && v == i);
  const std::size_t kept = me.retired_count;  // retired under the first mark
  g_pin_go = true;
  a.join();
  for (std::uint64_t i = 17; i < 20; ++i) q.push(i);
  std::thread b = held_push(20);
  for (std::uint64_t i = 16; i < 20; ++i) EXPECT(q.try_pop(v) && v == i);
  const std::size_t after = me.retired_count;
  g_pin_go = true;
  b.join();
  EXPECT(q.try_pop(v) && v == 20);
  std::printf("relay: %zu segments retired under the first mark, %zu waiting under the second\n", kept, after);
  EXPECT(kept >= 3);
  EXPECT(after <= 2);
}

void producers(unsigned p, long n) {
  wtc::rt::segment_queue<std::uint64_t, false, 8, 64, wtc::rt::no_hooks, kOne> q;
  std::vector<std::thread> ts;
  for (unsigned t = 0; t < p; ++t)
    ts.emplace_back([&, t] {
      std::uint64_t run[3];
      for (long s = 0; s < n;) {
        const long k = (s % 5 == 0 && s + 3 <= n) ? 3 : 1;
        for (long j = 0; j < k; ++j) run[j] = std::uint64_t{t} << 40 | static_cast<std::uint64_t>(s + j);
        q.push_run(run, static_cast<std::size_t>(k));
        s += k;
      }
    });
  std::vector<long> next(p, 0);
  long got = 0;
  std::uint64_t v;
  while (got < static_cast<long>(p) * n) {
    if (!q.try_pop(v)) continue;
    const unsigned t = static_cast<unsigned>(v >> 40);
    EXPECT(t < p && static_cast<long>(v & ((std::uint64_t{1} << 40) - 1)) == next[t]);
    if (t < p) ++next[t];
    ++got;
  }
  for (auto& t : ts) t.join();
  EXPECT(!q.try_pop(v));
  std::printf("producers: %u producers, %ld elements each, every one once and in order\n", p, n);
}
}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "churn";
  const long n = argc > 2 ? std::atol(argv[2]) : 10000;
  if (!std::strcmp(mode, "churn")) churn(n);
  else if (!std::strcmp(mode, "stalled")) stalled();
  else if (!std::strcmp(mode, "fork")) forked();
  else if (!std::strcmp(mode, "late")) late();
  else if (!std::strcmp(mode, "late_two")) late_two();
  else if (!std::strcmp(mode, "tail")) tail();
  else if (!std::strcmp(mode, "pin")) pin();
  else if (!std::strcmp(mode, "relay")) relay();
  else if (!std::strcmp(mode, "overlap"))
    overlap(argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 8, argc > 3 ? std::atoi(argv[3]) : 2);
  else if (!std::strcmp(mode, "nested")) nested();
  else if (!std::strcmp(mode, "deep")) deep();
  else if (!std::strcmp(mode, "producers"))
    producers(argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 4, argc > 3 ? std::atol(argv[3]) : 100000);
  else {
    std::fprintf(stderr, "hazard_test: unknown mode %s\n", mode);
    return 2;
  }
  if (failures) {
    std::fprintf(stderr, "hazard_test %s: %d failure(s)\n", mode, failures);
    return 1;
  }
  std::printf("hazard_test %s: ok\n", mode);
  return 0;
}
