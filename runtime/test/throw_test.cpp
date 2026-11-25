// A push whose element's move, or whose segment's allocation, throws after
// the push has taken its cells.
//
//   throw_test generic  the general-purpose queue: the move of one element
//                       throws; the push lets the exception out, and pops
//                       pass the cell it gave up rather than waiting for it
//                       for ever; every other element arrives once
//   throw_test batch    the same with a batch, the third of five elements
//                       throwing: the first two arrive, the cells after are
//                       given up
//   throw_test mpsc     the single-consumer queue, segments of four: the
//                       consumer passes the cell, and its head moves on, so
//                       the segments behind it are retired rather than kept
//                       behind a hole for ever
//   throw_test jiffy    the same with the one-counter push
//   throw_test hole     the single-consumer queue, a push held inside its
//                       element's move while the consumer takes the
//                       elements after it out of order, and then throwing:
//                       the consumer's out-of-order path passes the cell it
//                       gave up rather than taking what is not there
//   throw_test hole_jiffy  the same with the one-counter push
//   throw_test alloc    the single-consumer queue, the allocation of the
//                       segment a batch runs into failing: the push lets
//                       bad_alloc out, and its cells are given up, so the
//                       head passes them and retires segments (built
//                       without sanitizers, which replace operator new)
//   throw_test alloc_jiffy  the same with the one-counter push
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include "segment_queue.hpp"

namespace {
int failures = 0;

#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

std::atomic<int> g_throw_at{-1};
// The element whose move waits for g_release and then throws.
std::atomic<int> g_hold_at{-1};
std::atomic<bool> g_holding{false}, g_release{false};
struct elem {
  int v = 0;
  elem() = default;
  explicit elem(int x) : v(x) {}
  elem(elem&& o) : v(o.v) {
    if (v == g_hold_at.load()) {
      g_holding = true;
      while (!g_release.load()) std::this_thread::yield();
      throw std::runtime_error("held move");
    }
    if (v == g_throw_at.load()) throw std::runtime_error("move");
  }
  elem& operator=(elem&& o) {
    v = o.v;
    return *this;
  }
};

std::atomic<long> g_retires{0};
struct count_hooks : wtc::rt::no_hooks {
  static void retire() { g_retires.fetch_add(1); }
};

// Pops on another thread, for at most a few seconds: a pop that waits for
// a given-up cell for ever shows as elements missing.
template <class Q>
std::vector<int> drain(Q& q, std::size_t want) {
  std::vector<int> got;
  std::atomic<bool> done{false};
  std::thread c([&] {
    elem out;
    const auto t0 = std::chrono::steady_clock::now();
    while (got.size() < want && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
      if (q.try_pop(out)) got.push_back(out.v);
    done = true;
  });
  c.join();
  return got;
}

template <bool kMany>
void single(const char* what) {
  wtc::rt::segment_queue<elem, kMany, 8> q;
  for (int i = 0; i < 3; ++i) q.push(elem(i));
  g_throw_at = 3;
  bool threw = false;
  elem x(3);
  try {
    q.push_run(&x, 1);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  g_throw_at = -1;
  EXPECT(threw);
  for (int i = 4; i < 40; ++i) q.push(elem(i));
  const std::vector<int> got = drain(q, 39);
  std::printf("%s: the push threw; %zu of 39 others popped\n", what, got.size());
  EXPECT(got.size() == 39);
}

void batch() {
  wtc::rt::segment_queue<elem, true, 8> q;
  q.push(elem(0));
  elem run[5] = {elem(1), elem(2), elem(3), elem(4), elem(5)};
  g_throw_at = 3;
  bool threw = false;
  try {
    q.push_run(run, 5);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  g_throw_at = -1;
  EXPECT(threw);
  for (int i = 6; i < 30; ++i) q.push(elem(i));
  const std::vector<int> got = drain(q, 27);
  std::printf("batch: the push threw at its third; %zu of 27 popped\n", got.size());
  EXPECT(got.size() == 27);  // 0, 1, 2 and 6..29
}

template <bool kOne>
using one_consumer = wtc::rt::segment_queue<elem, false, 4, 64, count_hooks, kOne>;

template <bool kOne>
void mpsc(const char* name) {
  one_consumer<kOne> q;
  q.push(elem(0));
  g_throw_at = 1;
  bool threw = false;
  elem x(1);
  try {
    q.push_run(&x, 1);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  g_throw_at = -1;
  EXPECT(threw);
  elem out;
  int got = 0;
  for (int i = 2; i < 4000; ++i) {
    q.push(elem(i));
    if (q.try_pop(out)) ++got;
  }
  while (q.try_pop(out)) ++got;
  std::printf("%s: the push threw; %d of 3999 others popped, %ld segments retired\n", name, got, g_retires.load());
  EXPECT(got == 3999);
  // The head moved past the given-up cell, so the segments behind it were
  // retired, about one per four elements; behind a hole, none would be.
  EXPECT(g_retires.load() > 900);
}

template <bool kOne>
void hole(const char* name) {
  one_consumer<kOne> q;
  q.push(elem(0));
  g_hold_at = 1;
  std::atomic<bool> threw{false};
  std::thread p([&] {
    elem x(1);
    try {
      q.push_run(&x, 1);
    } catch (const std::runtime_error&) {
      threw = true;
    }
  });
  while (!g_holding.load()) std::this_thread::yield();
  q.push(elem(2));
  q.push(elem(3));
  elem out;
  std::vector<int> got;
  for (int k = 0; k < 3; ++k)
    if (q.try_pop(out)) got.push_back(out.v);
  EXPECT(got == (std::vector<int>{0, 2, 3}));  // 2 and 3 out of order, past the held push's cell
  g_release = true;
  p.join();
  g_hold_at = -1;
  EXPECT(threw.load());
  for (int i = 4; i < 40; ++i) q.push(elem(i));
  while (q.try_pop(out)) got.push_back(out.v);
  std::printf("%s: %zu popped, the held push's cell given up\n", name, got.size());
  EXPECT(got.size() == 39);
  for (std::size_t k = 3; k < got.size(); ++k) EXPECT(got[k] == static_cast<int>(k) + 1);
}

#ifdef WTC_TEST_FAIL_NEW
std::atomic<bool> g_fail_new{false};
}  // namespace
void* operator new(std::size_t n) {
  if (g_fail_new.exchange(false)) throw std::bad_alloc();
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a) {
  if (g_fail_new.exchange(false)) throw std::bad_alloc();
  const std::size_t al = static_cast<std::size_t>(a);
  if (void* p = std::aligned_alloc(al, (n + al - 1) / al * al)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
namespace {

template <bool kOne>
void alloc(const char* name) {
  one_consumer<kOne> q;
  for (int i = 0; i < 2; ++i) q.push(elem(i));  // index 1 links the second segment
  elem run[8] = {elem(2), elem(3), elem(4), elem(5), elem(6), elem(7), elem(8), elem(9)};
  bool threw = false;
  g_fail_new = true;  // the third segment's allocation, which index 8 needs
  try {
    q.push_run(run, 8);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  g_fail_new = false;
  elem out;
  int got = 0;
  const long before = g_retires.load();
  for (int i = 10; i < 400; ++i) {
    q.push(elem(i));
    if (q.try_pop(out)) ++got;
  }
  while (q.try_pop(out)) ++got;
  std::printf("%s: the push %s; %d popped, %ld segments retired\n", name, threw ? "threw bad_alloc" : "did not throw",
              got, g_retires.load() - before);
  EXPECT(threw);
  EXPECT(g_retires.load() - before > 90);  // the head passed the given-up cells
  // 0 and 1, and 10 to 399; and of the batch, what it filled before the
  // failure. With one counter its indices are 2 to 9: 2 and 3 in the first
  // segment, 4 to 7 in the second, which index 1 linked, and 8 and 9 need
  // a third, given up. With the segments' own counters it takes 2 and 3 of
  // the first and puts the rest in a segment of its own, whose allocation
  // fails.
  EXPECT(got == 2 + (kOne ? 6 : 2) + 390);
}
#endif
}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "generic";
  if (!std::strcmp(mode, "generic")) single<true>("generic");
  else if (!std::strcmp(mode, "mpsc")) mpsc<false>("mpsc");
  else if (!std::strcmp(mode, "jiffy")) mpsc<true>("jiffy");
  else if (!std::strcmp(mode, "hole")) hole<false>("hole");
  else if (!std::strcmp(mode, "hole_jiffy")) hole<true>("hole_jiffy");
  else if (!std::strcmp(mode, "batch")) batch();
#ifdef WTC_TEST_FAIL_NEW
  else if (!std::strcmp(mode, "alloc")) alloc<false>("alloc");
  else if (!std::strcmp(mode, "alloc_jiffy")) alloc<true>("alloc_jiffy");
#endif
  else {
    std::fprintf(stderr, "throw_test: unknown mode %s\n", mode);
    return 2;
  }
  if (failures) {
    std::fprintf(stderr, "throw_test %s: %d failure(s)\n", mode, failures);
    return 1;
  }
  std::printf("throw_test %s: ok\n", mode);
  return 0;
}
