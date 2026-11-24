// What the first operation of a thread, and of the process, does beyond
// its algorithm. Progress is claimed of an operation's own steps; a first
// use that takes a lock or spins for a measurement makes the claim false
// once per thread or once per process, which a real-time or signal-handler
// path observes.
//
//   tick_rate  the first catch-up of the spsc ring's and of the
//              single-consumer queue's consumer in the process: no
//              static-initialisation guard (__cxa_guard_acquire, wrapped
//              by the linker and counted) and well under the 200 µs the
//              counter's rate takes to measure, which the queue's
//              constructor has done.
//
// Built without sanitizers only: the counts come from wrapping the C++
// runtime's entry points, which a sanitizer's own interceptors would see
// as well.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "segment_queue.hpp"
#include "spsc_ring.hpp"

namespace {
thread_local bool t_on = false;
thread_local int t_guards = 0;
int failures = 0;

#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)
}  // namespace

// -Wl,--wrap=__cxa_guard_acquire: every guarded static initialisation this
// program's own code reaches.
extern "C" int __real___cxa_guard_acquire(std::uint64_t*);
extern "C" int __wrap___cxa_guard_acquire(std::uint64_t* g) {
  if (t_on) ++t_guards;
  return __real___cxa_guard_acquire(g);
}

namespace {
using clk = std::chrono::steady_clock;

struct probe_result {
  int guards;
  double us;
};

template <class F>
probe_result probe(const char* what, F&& f) {
  t_guards = 0;
  t_on = true;
  const auto t0 = clk::now();
  f();
  const auto t1 = clk::now();
  t_on = false;
  const probe_result r{t_guards, std::chrono::duration<double, std::micro>(t1 - t0).count()};
  std::printf("%-56s guards %d  %8.1f us\n", what, r.guards, r.us);
  return r;
}

// Under the 200 µs a measurement takes by a factor of twenty, and above
// what a preempted catch-up on a busy host is likely to take.
constexpr double kFirstCatchUpUs = 10.0;

void tick_rate() {
  // The queues are built here, which measures the rate; their first
  // catch-ups are then each the first of their kind in the process.
  wtc::rt::spsc_ring<std::uint64_t> r;
  wtc::rt::segment_queue<std::uint64_t, false> m;
  r.push(std::uint64_t{7});
  for (std::uint64_t i = 0; i < 200; ++i) m.push(i);
  std::thread([&] {
    std::uint64_t v = 0;
    probe_result p = probe("spsc pop, first catch-up in the process", [&] { r.try_pop(v); });
    EXPECT(v == 7);
    EXPECT(p.guards == 0);
    EXPECT(p.us < kFirstCatchUpUs);
    // The single consumer's first slip point is its first cell.
    p = probe("mpsc pop, first slip point in the process", [&] { m.try_pop(v); });
    EXPECT(v == 0);
    EXPECT(p.guards == 0);
    EXPECT(p.us < kFirstCatchUpUs);
  }).join();
  EXPECT(wtc::rt::ticks_per_ms() != 0);
}
}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "tick_rate";
  if (!std::strcmp(mode, "tick_rate")) tick_rate();
  else {
    std::fprintf(stderr, "first_use_test: unknown mode %s\n", mode);
    return 2;
  }
  if (failures) {
    std::fprintf(stderr, "first_use_test %s: %d failure(s)\n", mode, failures);
    return 1;
  }
  std::printf("first_use_test %s: ok\n", mode);
  return 0;
}
