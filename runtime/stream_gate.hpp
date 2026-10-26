// wtc::rt::stream_gate: whether a single consumer should wait for a batch.
//
// The spsc ring and the single-consumer segmented queue both let their
// consumer keep its distance from a fast producer: when it catches up it
// waits, boundedly, for a batch to fill, so that the lines it reads are
// done with and the producer's counter or state line crosses once per
// batch rather than once per element. That wait pays only for a producer
// that publishes faster than a line crosses between cores, about kStreamNs:
// a consumer's poll then costs it a transfer per element. A producer of
// 64 to 256 byte elements through the segmented queue takes about 60 ns an
// element, and without the wait ran at half its rate. For a paced
// producer, one element every few hundred nanoseconds or bursts with
// pauses between, the wait is pure latency: after a burst grew the batch,
// the next lone elements waited for it, and at a million elements a
// second up to half of them arrived about a microsecond late.
//
// So the consumer measures. At each catch-up it reads the processor's
// tick counter and adds the interval since the last catch-up, and the
// elements that arrived in it, to two running sums that decay by a
// quarter each time; the producer streams while the sums say less than
// kStreamNs per element. The interval counts the producer's idle time, so
// bursts with pauses between do not read as a stream.
//
// A measurement alone can lock the queue into the slow regime: a consumer
// that runs right behind the producer, with no wait, takes the counter's
// line from it on every element and can slow it past kStreamNs an
// element, which then never reads as a stream. So after kProbeEvery
// catch-ups that do not read as a stream, the consumer waits once anyway,
// for a batch of two; if the batch fills during that bounded wait, the
// producer may have been held back by the polls: the time measured so far
// is dropped and the arrivals seeded, so that a batch that filled fast
// opens the gate, which then stays open while the producer, now left
// alone, streams. Each probe doubles the count to the next, up to
// kProbeMax, and kStayOpen catch-ups in a row that read as a stream bring
// it back to kProbeEvery. A paced producer, which a probe does not keep
// open, so pays a handful of waits and then one per kProbeMax catch-ups,
// too few to show at its 99th percentile; a consumer stuck behind a fast
// producer gets out within kProbeMax catch-ups of entering, at once if it
// enters from a stream.
//
// The tick counter's rate is measured once per process, on first use,
// against the steady clock, rather than assumed: a time-stamp counter runs
// at the processor's nominal clock, from under 2 GHz to over 4.

#ifndef WTC_RUNTIME_STREAM_GATE_HPP
#define WTC_RUNTIME_STREAM_GATE_HPP

#include <chrono>
#include <cstdint>

namespace wtc::rt {

inline std::uint64_t ticks() {
#if defined(__x86_64__) || defined(__i386__)
  return __builtin_ia32_rdtsc();
#elif defined(__aarch64__)
  std::uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
#else
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// Counter ticks per millisecond. On x86 measured against the steady
// clock: each end of a 200 µs interval is a clock read bracketed by two
// counter reads, the narrowest bracket of sixteen, so a preemption between
// the clock and the counter cannot bias the rate for the process's life.
// AArch64's counter states its frequency.
inline std::uint64_t measure_ticks_per_ms() {
#if defined(__x86_64__) || defined(__i386__)
  using clk = std::chrono::steady_clock;
  const auto paired = [](std::uint64_t& tick) {
    clk::time_point at{};
    std::uint64_t width = ~std::uint64_t{0};
    for (int k = 0; k < 16; ++k) {
      const std::uint64_t a = ticks();
      const clk::time_point c = clk::now();
      const std::uint64_t b = ticks();
      if (b >= a && b - a < width) {
        width = b - a;
        tick = a + (b - a) / 2;
        at = c;
      }
    }
    return at;
  };
  std::uint64_t t0 = 0, t1 = 0;
  const clk::time_point c0 = paired(t0);
  while (clk::now() - c0 < std::chrono::microseconds(200)) {
  }
  const clk::time_point c1 = paired(t1);
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count();
  const std::uint64_t r =
      ns > 0 && t1 > t0 ? (t1 - t0) * 1000000 / static_cast<std::uint64_t>(ns) : 0;
  return r ? r : 1;
#elif defined(__aarch64__)
  std::uint64_t f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  const std::uint64_t r = f / 1000u;
  return r ? r : 1;
#else
  return 1000000;  // the fallback counter is the steady clock, in nanoseconds
#endif
}

// Measured on first use, once per process, by whichever queue first needs
// it; nothing runs at program start.
inline std::uint64_t ticks_per_ms() {
  static const std::uint64_t rate = measure_ticks_per_ms();
  return rate;
}

class stream_gate {
public:
  static constexpr std::uint64_t kStreamNs = 100;
  static constexpr unsigned kProbeEvery = 256;
  static constexpr unsigned kProbeMax = 65536;
  static constexpr unsigned kStayOpen = 8;
  // Elements a filled probe adds to the arrivals: enough for the interval
  // the probe's wait fell in to read as a stream when the batch filled as
  // fast as a held-back producer fills it, a few hundred nanoseconds with
  // the drain, and not when it filled as a paced producer does.
  static constexpr std::uint64_t kProbeSeed = 4;
  stream_gate() = default;
  // With a given counter rate, for tests that drive the gate with their
  // own tick sequence through should_wait_at.
  explicit stream_gate(std::uint64_t ticks_per_ms) : thr_(kStreamNs * ticks_per_ms) {}

  // At a catch-up, with the number of elements that arrived since the
  // last one. True when the consumer should wait for a batch: the producer
  // streams, or a probe is due (probing() then says which).
  [[gnu::noinline]] bool should_wait(std::uint64_t arrived) {
    return should_wait_at(arrived, ticks());
  }

  bool should_wait_at(std::uint64_t arrived, std::uint64_t now) {
    if (thr_ == 0) thr_ = kStreamNs * ticks_per_ms();
    // A counter read on another core can be behind: skip such a sample.
    // An unchanged counter is a zero interval, which a coarse counter
    // gives often; its arrivals still count.
    if (started_ && now >= t_) {
      std::uint64_t dt = now - t_;
      if (dt > kMaxInterval) dt = kMaxInterval;
      t_sum_ = t_sum_ - (t_sum_ >> 2) + (dt << kFrac);
      a_sum_ = a_sum_ - (a_sum_ >> 2) + (arrived << kFrac);
    }
    t_ = now;
    started_ = true;
    probing_ = false;
    // Under kStreamNs an element: t_sum / a_sum ticks against
    // kStreamNs * ticks_per_ms / 10^6, compared without dividing.
    if (a_sum_ != 0 && t_sum_ * 1000000 <= thr_ * a_sum_) {
      ++streams_;
      probe_ = 0;
      if (open_run_ < kStayOpen && ++open_run_ == kStayOpen) probe_every_ = kProbeEvery;
      return true;
    }
    open_run_ = 0;
    if (++probe_ >= probe_every_) {
      probe_ = 0;
      if (probe_every_ < kProbeMax) probe_every_ *= 2;
      probing_ = true;
      ++probes_;
      return true;
    }
    return false;
  }

  bool probing() const { return probing_; }

  // After a wait: did the batch fill? A probe that filled may mean the
  // consumer's polls were holding the producer back: the time measured so
  // far is dropped and the arrivals seeded, so the next catch-up opens the
  // gate if the batch filled fast; from there the producer, no longer held
  // back, shows its rate, and the gate stays open while it streams.
  void waited(bool filled) {
    if (probing_ && filled) {
      t_sum_ = 0;
      a_sum_ += kProbeSeed << kFrac;
    }
    probing_ = false;
  }

  // Catch-ups that read as a stream, and probes; hints for tests.
  std::uint64_t streams() const { return streams_; }
  std::uint64_t probes() const { return probes_; }

private:
  // One to two seconds at the rates counters run at: keeps the sums finite
  // after a long idle period without making it look short.
  static constexpr std::uint64_t kMaxInterval = std::uint64_t{1} << 32;
  // The sums carry four fraction bits, so that their decay by a quarter
  // does not stall at the few ticks a coarse counter gives per catch-up.
  static constexpr unsigned kFrac = 4;

  std::uint64_t thr_ = 0;  // kStreamNs * ticks per ms, once known
  std::uint64_t t_ = 0;
  bool started_ = false;
  std::uint64_t t_sum_ = 0;
  std::uint64_t a_sum_ = 0;
  std::uint64_t streams_ = 0;
  std::uint64_t probes_ = 0;
  unsigned probe_ = 0;              // catch-ups since the last probe or stream
  unsigned probe_every_ = kProbeEvery;
  unsigned open_run_ = 0;           // stream catch-ups in a row, up to kStayOpen
  bool probing_ = false;
};

} // namespace wtc::rt

#endif // WTC_RUNTIME_STREAM_GATE_HPP
