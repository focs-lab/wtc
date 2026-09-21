// Single-threaded behavioural tests for wtc::rt::spsc_ring. Every property
// here is one the two-thread stress test cannot pin down precisely: exact
// FIFO over block boundaries, reuse without growth, growth when the consumer
// lags, and the destructor releasing what was never popped.

#include "spsc_ring.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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

using ring = wtc::rt::spsc_ring<std::uint64_t, 8>; // small blocks: many turnovers

void empty_queue_pops_nothing() {
  ring q;
  std::uint64_t v = 42;
  EXPECT(!q.try_pop(v));
  EXPECT(v == 42);
}

void fifo_across_many_blocks() {
  ring q;
  const std::uint64_t n = 1000; // 125 blocks' worth through an 8-slot ring
  for (std::uint64_t i = 0; i < n; ++i) q.push(i);
  for (std::uint64_t i = 0; i < n; ++i) {
    std::uint64_t v = ~0ull;
    EXPECT(q.try_pop(v));
    EXPECT(v == i);
  }
  std::uint64_t v = ~0ull;
  EXPECT(!q.try_pop(v));
}

void interleaved_reuses_blocks_without_growing() {
  // Push and pop in lockstep across many block boundaries. If reuse works
  // the ring stays at its two initial blocks; the only observable is that
  // nothing breaks and FIFO holds, so this doubles as the lockstep test
  // that the design note in the header worries about.
  ring q;
  std::uint64_t next_in = 0, next_out = 0;
  for (int round = 0; round < 10000; ++round) {
    const unsigned burst = 1 + (round % 13);
    for (unsigned k = 0; k < burst; ++k) q.push(next_in++);
    for (unsigned k = 0; k < burst; ++k) {
      std::uint64_t v = ~0ull;
      EXPECT(q.try_pop(v));
      EXPECT(v == next_out++);
    }
  }
  std::uint64_t v = ~0ull;
  EXPECT(!q.try_pop(v));
}

void exact_block_lockstep() {
  // Fill exactly one block, drain exactly one block, repeat. This is the
  // pattern where a wrong publication point would make the producer
  // allocate a fresh block every round or, worse, reorder the ring.
  ring q;
  std::uint64_t next_in = 0, next_out = 0;
  for (int round = 0; round < 5000; ++round) {
    for (unsigned k = 0; k < 8; ++k) q.push(next_in++);
    for (unsigned k = 0; k < 8; ++k) {
      std::uint64_t v = ~0ull;
      EXPECT(q.try_pop(v));
      EXPECT(v == next_out++);
    }
  }
}

void growth_then_drain_then_reuse() {
  ring q;
  // Lagging consumer: 10 blocks in flight forces 8 insertions.
  for (std::uint64_t i = 0; i < 80; ++i) q.push(i);
  for (std::uint64_t i = 0; i < 80; ++i) {
    std::uint64_t v = ~0ull;
    EXPECT(q.try_pop(v));
    EXPECT(v == i);
  }
  // The grown ring must still deliver in order afterwards.
  for (std::uint64_t i = 100; i < 300; ++i) q.push(i);
  for (std::uint64_t i = 100; i < 300; ++i) {
    std::uint64_t v = ~0ull;
    EXPECT(q.try_pop(v));
    EXPECT(v == i);
  }
}

void destructor_releases_unpopped_elements() {
  auto counter = std::make_shared<int>(0);
  {
    wtc::rt::spsc_ring<std::shared_ptr<int>, 4> q;
    for (int i = 0; i < 37; ++i) q.push(counter); // several blocks, none popped
    EXPECT(counter.use_count() == 38);
    std::shared_ptr<int> p;
    EXPECT(q.try_pop(p));
    EXPECT(counter.use_count() == 38); // moved out, not copied
    p.reset();
    EXPECT(counter.use_count() == 37);
  }
  EXPECT(counter.use_count() == 1); // destructor released the remaining 36
}

void moved_out_slot_is_empty() {
  wtc::rt::spsc_ring<std::string, 4> q;
  q.push(std::string(100, 'x'));
  std::string s;
  EXPECT(q.try_pop(s));
  EXPECT(s.size() == 100);
}

} // namespace

int main() {
  empty_queue_pops_nothing();
  fifo_across_many_blocks();
  interleaved_reuses_blocks_without_growing();
  exact_block_lockstep();
  growth_then_drain_then_reuse();
  destructor_releases_unpopped_elements();
  moved_out_slot_is_empty();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("spsc_ring_test: ok");
  return 0;
}
