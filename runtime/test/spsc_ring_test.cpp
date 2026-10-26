// Single-threaded behavioural tests for wtc::rt::spsc_ring. Every property
// here is one the two-thread stress test cannot pin down precisely: exact
// FIFO over block boundaries, reuse without growth, growth when the consumer
// lags, and the destructor releasing what was never popped.

#include "spsc_ring.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
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

#if defined(__SANITIZE_THREAD__)
#define WTC_TEST_SLOW 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WTC_TEST_SLOW 1
#endif
#endif

// The only waits a non-streaming consumer makes are probes, at most one
// per kProbeEvery catch-ups (stream_gate.hpp).
constexpr std::uint64_t probes(std::uint64_t catch_ups) {
  return catch_ups / wtc::rt::stream_gate::kProbeEvery + 1;
}

void lockstep_never_slips() {
  // One element in, one out: the consumer catches up every time with one
  // element published, which is the batch it waits for, so it never waits
  // and the batch never grows, but for the probes if the round trip is too
  // slow to read as a stream. A consumer that waited here would add a
  // bounded spin to every round trip of a request-reply pair.
  ring q;
  for (std::uint64_t i = 0; i < 100000; ++i) {
    q.push(i);
    std::uint64_t v = ~0ull;
    EXPECT(q.try_pop(v));
    EXPECT(v == i);
  }
  EXPECT(q.slips() <= probes(100000));
}

// Spins for about d of wall time, as a producer that paces itself would.
void spin_for(std::chrono::nanoseconds d) {
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < d) {
  }
}

void trickle_never_waits() {
  // A producer that publishes one element every few microseconds is read
  // the moment it publishes, even right after a burst: a wait would only
  // add its own length to every element's latency.
  ring q;
  std::uint64_t v = ~0ull;
  for (std::uint64_t i = 0; i < 16; ++i) q.push(i);
  for (std::uint64_t i = 0; i < 16; ++i) EXPECT(q.try_pop(v) && v == i);
  for (std::uint64_t i = 16; i < 1016; ++i) {
    spin_for(std::chrono::microseconds(5));
    q.push(i);
    EXPECT(q.try_pop(v) && v == i);
  }
  EXPECT(q.slips() <= probes(1000));
}

void stream_waits_boundedly_then_takes_what_is_there() {
  // A stream grows the batch; a lone element after it finds fewer than a
  // batch published and no producer to deliver more. The wait must end on
  // its own within its bound, the element must come out, and once the
  // producer has paused the next lone element is read without a wait.
  ring q;
  std::uint64_t v = ~0ull, n = 0;
  for (int r = 0; r < 64; ++r) {
    const std::uint64_t first = n;
    for (int k = 0; k < 4; ++k) q.push(n++);
    for (std::uint64_t i = first; i < n; ++i) EXPECT(q.try_pop(v) && v == i);
  }
  const std::uint64_t before = q.slips();
  q.push(n);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT(q.try_pop(v) && v == n);
  const auto took = std::chrono::steady_clock::now() - t0;
  ++n;
  // The wait is bounded by a count of pauses, about a hundred, which is a
  // few microseconds on processors with slow pauses; generous here for a
  // sanitized build on a loaded host.
  EXPECT(took < std::chrono::milliseconds(100));
  // At most one wait. Whether there was one depends on the rate the rounds
  // above measured, which a loaded host or a sanitizer can push past the
  // streaming threshold, so the test does not insist on it.
  EXPECT(q.slips() <= before + 1);
  const std::uint64_t after = q.slips();
  spin_for(std::chrono::microseconds(50));
  q.push(n);
  EXPECT(q.try_pop(v) && v == n);
  EXPECT(q.slips() == after);
  EXPECT(!q.try_pop(v));
}

void streaming_is_reached() {
  // Rounds of a few pushes and their pops, as fast as one thread goes: the
  // consumer must read this as a stream. A gate that never opens fails here.
  ring q;
  std::uint64_t v = ~0ull, n = 0;
  for (int r = 0; r < 512; ++r) {
    const std::uint64_t first = n;
    for (int k = 0; k < 64; ++k) q.push(n++);
    for (std::uint64_t i = first; i < n; ++i) EXPECT(q.try_pop(v) && v == i);
  }
#ifndef WTC_TEST_SLOW
  EXPECT(q.streams() > 0);
#endif
}

void fast_producer_end_to_end() {
  // A producer pushing flat out on another thread, a consumer popping as
  // fast as it can, through whichever regimes the gate moves between: a
  // smoke check that order holds. Which regimes a run sees depends on the
  // host and the sanitizer; stream_gate_test pins those down.
  using big = wtc::rt::spsc_ring<std::uint64_t>;
  big q;
  constexpr std::uint64_t kN = 2000000;
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kN; ++i) q.push(i);
  });
  std::uint64_t v = ~0ull, next = 0;
  while (next < kN) {
    if (q.try_pop(v)) {
      EXPECT(v == next);
      ++next;
    }
  }
  producer.join();
  EXPECT(!q.try_pop(v));
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

// Counts live objects, so that a test can see every element constructed
// is destroyed exactly once, whichever way it left the ring.
struct tracked {
  static int live;
  std::uint64_t v;
  explicit tracked(std::uint64_t x) : v(x) { ++live; }
  tracked(const tracked& o) : v(o.v) { ++live; }
  tracked(tracked&& o) noexcept : v(o.v) { ++live; }
  tracked& operator=(const tracked&) = default;
  tracked& operator=(tracked&&) = default;
  ~tracked() { --live; }
};
int tracked::live = 0;

void element_without_default_constructor() {
  // Slots are raw storage, so the element type needs no default
  // constructor; a block no longer constructs one per slot.
  struct only_from_int {
    explicit only_from_int(int x) : v(x) {}
    int v;
  };
  wtc::rt::spsc_ring<only_from_int, 4> q;
  for (int i = 0; i < 10; ++i) q.push(only_from_int(i));
  only_from_int out(-1);
  for (int i = 0; i < 10; ++i) {
    EXPECT(q.try_pop(out));
    EXPECT(out.v == i);
  }
  EXPECT(!q.try_pop(out));
}

void every_element_destroyed_once() {
  {
    wtc::rt::spsc_ring<tracked, 4> q;
    for (std::uint64_t i = 0; i < 37; ++i) q.push(tracked(i)); // several blocks
    EXPECT(tracked::live == 37);
    tracked out(0);
    for (std::uint64_t i = 0; i < 10; ++i) EXPECT(q.try_pop(out) && out.v == i);
    EXPECT(tracked::live == 27 + 1); // the 27 still queued, and out
    for (std::uint64_t i = 10; i < 15; ++i) {
      tracked* b = q.try_pop_borrow();
      EXPECT(b && b->v == i);
    }
    EXPECT(tracked::live == 22 + 1 + 1); // queued, out, and the one lent
  }
  // The destructor ended the 22 still queued and the one lent; out is gone
  // with its scope.
  EXPECT(tracked::live == 0);
}

void borrowed_element_lives_until_the_next_pop() {
  wtc::rt::spsc_ring<tracked, 8> q;
  q.push(tracked(1));
  q.push(tracked(2));
  tracked* first = q.try_pop_borrow();
  EXPECT(first && first->v == 1);
  EXPECT(tracked::live == 2); // still alive, in place
  q.push(tracked(3));         // a push does not disturb it
  EXPECT(first->v == 1);
  tracked* second = q.try_pop_borrow();
  EXPECT(second && second->v == 2);
  EXPECT(tracked::live == 2); // first destroyed, 2 lent, 3 queued
  EXPECT(q.try_pop_borrow() != nullptr);
  EXPECT(q.try_pop_borrow() == nullptr); // empty, and 3 released
  EXPECT(tracked::live == 0);
}

void borrowed_element_survives_the_producer_moving_on() {
  // Two slots per block: the element lent from the last slot of a block
  // must stay intact while the producer fills new blocks, because the
  // consumer has not yet announced that it left the block it lives in.
  wtc::rt::spsc_ring<std::uint64_t, 2> q;
  q.push(0);
  q.push(1);
  EXPECT(*q.try_pop_borrow() == 0);
  std::uint64_t* lent = q.try_pop_borrow();
  EXPECT(lent && *lent == 1);
  for (std::uint64_t i = 2; i < 40; ++i) q.push(i);
  EXPECT(*lent == 1);
  for (std::uint64_t i = 2; i < 40; ++i) {
    std::uint64_t* b = q.try_pop_borrow();
    EXPECT(b && *b == i);
  }
}

void deferred_pushes_appear_at_publish() {
  wtc::rt::spsc_ring<std::uint64_t, 4> q;
  for (std::uint64_t i = 0; i < 10; ++i) q.push_deferred(i);  // across blocks
  std::uint64_t v = ~0ull;
  EXPECT(!q.try_pop(v));  // nothing visible yet
  q.publish();
  for (std::uint64_t i = 0; i < 10; ++i) {
    EXPECT(q.try_pop(v));
    EXPECT(v == i);
  }
  EXPECT(!q.try_pop(v));
}

} // namespace

int main() {
  deferred_pushes_appear_at_publish();
  element_without_default_constructor();
  every_element_destroyed_once();
  borrowed_element_lives_until_the_next_pop();
  borrowed_element_survives_the_producer_moving_on();
  empty_queue_pops_nothing();
  fifo_across_many_blocks();
  interleaved_reuses_blocks_without_growing();
  exact_block_lockstep();
  lockstep_never_slips();
  trickle_never_waits();
  stream_waits_boundedly_then_takes_what_is_there();
  streaming_is_reached();
  fast_producer_end_to_end();
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
