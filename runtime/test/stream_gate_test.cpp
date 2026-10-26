// Deterministic tests for wtc::rt::stream_gate: the gate is driven with
// tick sequences written here rather than with the processor's counter, so
// each regime it must tell apart is reproduced exactly, whatever the host's
// load or a sanitizer does to the timing.

#include "stream_gate.hpp"

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

using wtc::rt::stream_gate;

// A consumer's catch-ups in nanoseconds of real time, read through a
// counter of a given rate as the gate would read the processor's.
struct driver {
  stream_gate gate;
  std::uint64_t ticks_per_ms;
  std::uint64_t ns = 1000000;
  std::uint64_t waits = 0, probes = 0;

  explicit driver(std::uint64_t rate) : gate(rate), ticks_per_ms(rate) {}

  // The next catch-up comes `gap` ns after the last one with `arrived`
  // elements; a wait it starts fills when `fills` says so. Returns whether
  // the gate read the producer as streaming.
  bool catch_up(std::uint64_t gap, std::uint64_t arrived, bool fills = false) {
    ns += gap;
    const std::uint64_t before = gate.streams();
    if (gate.should_wait_at(arrived, ns * ticks_per_ms / 1000000)) {
      ++waits;
      if (gate.probing()) ++probes;
      gate.waited(fills);
    }
    return gate.streams() != before;
  }
};

constexpr std::uint64_t kNsCounter = 1000000;  // one tick a nanosecond
constexpr std::uint64_t kCoarseCounter = 19200; // 19.2 MHz, as on AArch64
constexpr std::uint64_t kMhzCounter = 1000;     // 1 MHz, a tick per µs

// Probes in n catch-ups that never read as a stream: the first after
// kProbeEvery, each next one twice as far, up to kProbeMax apart.
constexpr std::uint64_t backed_off_probes(std::uint64_t n) {
  std::uint64_t count = 0, at = 0, every = stream_gate::kProbeEvery;
  while (at + every <= n) {
    at += every;
    ++count;
    if (every < stream_gate::kProbeMax) every *= 2;
  }
  return count;
}

void paced_trickle_stays_closed() {
  // One element every 500 ns: the gate never reads it as a stream, and the
  // only waits are the probes, none of which fills, and which back off: a
  // dozen in the first million catch-ups, under two in a hundred thousand
  // and so well clear of the 99th percentile, then one per kProbeMax.
  driver d(kNsCounter);
  std::uint64_t opened = 0;
  for (int i = 0; i < 1000000; ++i) opened += d.catch_up(500, 1);
  EXPECT(opened == 0);
  EXPECT(d.probes == d.waits);
  EXPECT(d.probes == backed_off_probes(1000000));
  EXPECT(d.probes <= 24);
}

void bursts_with_pauses_stay_closed() {
  // Sixteen elements at once, then 10 µs idle: 625 ns an element on
  // average, however fast each burst is.
  driver d(kNsCounter);
  std::uint64_t opened = 0;
  for (int i = 0; i < 10000; ++i) opened += d.catch_up(10000, 16);
  EXPECT(opened == 0);
}

void stream_opens() {
  // Batches of 64 every 640 ns, 10 ns an element: open from the second
  // catch-up on, the first having nothing to measure against.
  driver d(kNsCounter);
  std::uint64_t opened = 0;
  for (int i = 0; i < 10000; ++i) opened += d.catch_up(640, 64);
  EXPECT(opened == 10000 - 1);
  EXPECT(d.probes == 0);
}

void stuck_closed_opens_only_at_the_probe_and_stays_open() {
  // A consumer right behind the producer takes one element per catch-up
  // and slows the producer to 200 ns an element: closed. Only the probe
  // opens it. Its batch of two fills at once, but the interval it falls
  // in also holds the wait's exit and the drain, 600 ns on a processor with
  // slow pauses, which the seed must carry; after that the producer, left
  // alone, streams at 10 ns an element and the gate stays open for good.
  driver d(kNsCounter);
  std::uint64_t opened = 0;
  for (unsigned i = 0; i + 1 < stream_gate::kProbeEvery; ++i) opened += d.catch_up(200, 1, true);
  EXPECT(opened == 0);
  EXPECT(d.probes == 0);
  d.catch_up(200, 1, true); // the probe, and its batch fills
  EXPECT(d.probes == 1);
  EXPECT(d.catch_up(600, 2, true)); // the probe's interval
  std::uint64_t batch = 2;
  opened = 0;
  for (int i = 0; i < 100000; ++i) {
    opened += d.catch_up(10 * batch, batch, true);
    if (batch < 64) batch *= 2;
  }
  EXPECT(opened == 100000);
  EXPECT(d.probes == 1);
}

// Catch-ups of a consumer stuck right behind the producer, 200 ns an
// element, until a probe comes, up to `limit`; then the probe's interval,
// which must open the gate.
void stuck_until_probed(driver& d, std::uint64_t limit) {
  const std::uint64_t before = d.probes;
  std::uint64_t n = 0;
  while (d.probes == before && n < limit) {
    d.catch_up(200, 1, true);
    ++n;
  }
  EXPECT(d.probes == before + 1);
  EXPECT(d.catch_up(600, 2, true));
}

void stuck_again_after_a_stream_is_probed_soon() {
  // Paced for long enough that the probes are kProbeMax apart, then a
  // stream, then stuck behind the producer again: the stream brought the
  // probes back to kProbeEvery, so the consumer gets out about as fast as
  // the first time (the few catch-ups the decaying sums still read as a
  // stream come first).
  driver d(kNsCounter);
  for (int i = 0; i < 1000000; ++i) d.catch_up(500, 1);
  for (int i = 0; i < 64; ++i) d.catch_up(640, 64, true);
  stuck_until_probed(d, 64 + stream_gate::kProbeEvery);
}

void stuck_after_a_long_paced_run_is_probed_within_the_cap() {
  // Without a stream between, a consumer that gets stuck after a long
  // paced run waits for the next probe, at most kProbeMax catch-ups, and
  // is then out for good.
  driver d(kNsCounter);
  for (int i = 0; i < 1000000; ++i) d.catch_up(500, 1);
  stuck_until_probed(d, stream_gate::kProbeMax);
  std::uint64_t opened = 0;
  for (int i = 0; i < 1000; ++i) opened += d.catch_up(320, 32, true);
  EXPECT(opened == 1000);
}

void filled_probe_on_a_paced_producer_stays_closed() {
  // A paced producer fills a probe's batch too, given time: one element
  // every microsecond does within a wait of about that. The gate must not
  // open on it, or every probe would cost a second wait; at 500 ns an
  // element at most one per probe. Neither stays open, so their probes
  // back off as if none had filled.
  driver slow(kNsCounter), paced(kNsCounter);
  std::uint64_t slow_opened = 0, paced_opened = 0;
  for (int i = 0; i < 10000; ++i) {
    slow_opened += slow.catch_up(1000, 1, true);
    paced_opened += paced.catch_up(500, 1, true);
  }
  EXPECT(slow_opened == 0);
  EXPECT(slow.probes == backed_off_probes(10000));
  EXPECT(paced_opened <= paced.probes);
  EXPECT(paced.probes <= backed_off_probes(10000));
}

void coarse_counter_reads_a_stream() {
  // At 19.2 MHz a tick is 52 ns, and a stream's catch-ups mostly land on
  // the tick of the one before: those are zero intervals whose arrivals
  // count.
  const std::uint64_t gaps[] = {10, 20, 30};
  for (const std::uint64_t gap : gaps) {
    driver d(kCoarseCounter);
    std::uint64_t opened = 0;
    for (int i = 0; i < 10000; ++i) opened += d.catch_up(gap, 1);
    EXPECT(opened >= 10000 - 8);
  }
  // A counter whose tick is longer than the threshold sees a stream only
  // through those zero intervals: dropped, they would leave one element
  // counted per microsecond, and the gate would never open. Counted, it
  // is open but for the few catch-ups after each tick.
  for (const std::uint64_t gap : gaps) {
    driver d(kMhzCounter);
    std::uint64_t opened = 0;
    for (int i = 0; i < 10000; ++i) opened += d.catch_up(gap, 1);
    EXPECT(opened >= 8000);
  }
}

void coarse_counter_keeps_a_paced_producer_closed() {
  // The threshold, 100 ns, is under two coarse ticks and is kept in
  // fractions of one, so a producer just above it is not taken for a
  // stream.
  const std::uint64_t gaps[] = {120, 150, 250};
  for (const std::uint64_t gap : gaps) {
    driver d(kCoarseCounter);
    std::uint64_t opened = 0;
    for (int i = 0; i < 10000; ++i) opened += d.catch_up(gap, 1);
    EXPECT(opened == 0);
  }
}

void counter_behind_is_skipped() {
  // A counter read on another core can be behind the last one: the sample
  // is dropped, not taken as an interval of nearly 2^64 ticks.
  stream_gate g(kNsCounter);
  std::uint64_t t = 1000000;
  for (int i = 0; i < 64; ++i) g.should_wait_at(16, t += 160);
  const std::uint64_t before = g.streams();
  EXPECT(g.should_wait_at(16, t - 5000));
  EXPECT(g.should_wait_at(16, t - 5000 + 160));
  EXPECT(g.streams() == before + 2);
}

} // namespace

int main() {
  paced_trickle_stays_closed();
  bursts_with_pauses_stay_closed();
  stream_opens();
  stuck_closed_opens_only_at_the_probe_and_stays_open();
  stuck_again_after_a_stream_is_probed_soon();
  stuck_after_a_long_paced_run_is_probed_within_the_cap();
  filled_probe_on_a_paced_producer_stays_closed();
  coarse_counter_reads_a_stream();
  coarse_counter_keeps_a_paced_producer_closed();
  counter_behind_is_skipped();
  if (failures) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return 1;
  }
  std::puts("stream_gate_test: ok");
  return 0;
}
