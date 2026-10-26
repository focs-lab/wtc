// Single-threaded checks of the single-consumer segmented queue's slip:
// a paced producer is read without waits, a fast one is read as a stream.

#include "segment_queue.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

#define EXPECT(cond)                                                          \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__,  \
                   #cond);                                                    \
      ++failures;                                                             \
    }                                                                         \
  } while (0)

#if defined(__SANITIZE_THREAD__)
#define WTC_TEST_SLOW 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WTC_TEST_SLOW 1
#endif
#endif

using one_consumer = wtc::rt::segment_queue<std::uint64_t, false>;

void spin_for(std::chrono::nanoseconds d) {
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < d) {
  }
}

void trickle_after_a_burst_never_waits() {
  // A burst grows the batch; then one element every few microseconds. The
  // consumer must read each the moment it is published: a wait at every
  // slip point would hold one element in thirty-two for its whole bound.
  // The only waits allowed are the probes, at most one per kProbeEvery
  // slip points.
  one_consumer q;
  std::uint64_t v = ~0ull, n = 0;
  for (int r = 0; r < 64; ++r) {
    const std::uint64_t first = n;
    for (int k = 0; k < 256; ++k) q.push(n++);
    for (std::uint64_t i = first; i < n; ++i) EXPECT(q.try_pop(v) && v == i);
  }
  const std::uint64_t before = q.slips();
  constexpr std::uint64_t kTrickle = 4096;
  for (std::uint64_t k = 0; k < kTrickle; ++k) {
    spin_for(std::chrono::microseconds(5));
    q.push(n);
    EXPECT(q.try_pop(v) && v == n);
    ++n;
  }
  const std::uint64_t slip_points = kTrickle / 32 + 1;
  EXPECT(q.slips() - before <= slip_points / wtc::rt::stream_gate::kProbeEvery + 1);
  EXPECT(!q.try_pop(v));
}

void fast_rounds_read_as_a_stream() {
  // Rounds of pushes and their pops as fast as one thread goes: a gate
  // that never opens fails here.
  one_consumer q;
  std::uint64_t v = ~0ull, n = 0;
  for (int r = 0; r < 256; ++r) {
    const std::uint64_t first = n;
    for (int k = 0; k < 256; ++k) q.push(n++);
    for (std::uint64_t i = first; i < n; ++i) EXPECT(q.try_pop(v) && v == i);
  }
#ifndef WTC_TEST_SLOW
  EXPECT(q.streams() > 0);
#endif
}

} // namespace

int main() {
  trickle_after_a_burst_never_waits();
  fast_rounds_read_as_a_stream();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("segment_queue_test: ok");
  return 0;
}
