// Shared pieces of the lemma tests (docs/correctness.md, section 3): an
// element type that counts its live copies and can hold the thread that
// moves it, gates that hold a thread at a named point until the test lets
// it go, and a check of every "empty" answer against the pushes that had
// finished before the pop began.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace lemma {

inline std::atomic<int> failures{0};

#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++lemma::failures;                                                     \
    }                                                                        \
  } while (0)

// A point a thread stops at: the thread announces it is there and waits
// for go. One-shot: reset() before reuse.
struct gate {
  std::atomic<bool> at{false};
  std::atomic<bool> go{false};
  void reset() {
    at.store(false);
    go.store(false);
  }
  void stop_here() {
    at.store(true, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
  }
  void wait_arrival() const {
    while (!at.load(std::memory_order_acquire)) std::this_thread::yield();
  }
  void release() { go.store(true, std::memory_order_release); }
};

inline void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Whether a thread is still inside a call after a while: the test's way of
// saying "this pop waits", with no claim about how long it would.
inline bool still_running(const std::atomic<bool>& done, int ms = 100) {
  sleep_ms(ms);
  return !done.load(std::memory_order_acquire);
}

// The element. id selects the element whose move a test holds: the
// stall_at-th move out of an element with that id stops at stall_gate.
inline std::atomic<long> live{0};
inline std::atomic<int> stall_id{0};
inline std::atomic<int> stall_at{0};
inline std::atomic<int> stall_moves{0};
inline gate stall_gate;

inline void arm_stall(int id, int at) {
  stall_gate.reset();
  stall_moves.store(0);
  stall_at.store(at);
  stall_id.store(id);
}

// A producer that sets this is held now and then, at random, in the
// middle of a move: between taking a cell and filling it.
inline thread_local unsigned move_jitter = 0;

inline void moved_out_of(int id) {
  if (move_jitter) {
    move_jitter = move_jitter * 1103515245u + 12345u;
    if ((move_jitter >> 16) % 53 == 0) std::this_thread::yield();
  }
  if (id == 0 || id != stall_id.load(std::memory_order_relaxed)) return;
  if (stall_moves.fetch_add(1) + 1 == stall_at.load()) {
    stall_id.store(0);
    stall_gate.stop_here();
  }
}

struct item {
  int id = 0;      // nonzero for elements a test may hold in a move
  int producer = 0;
  long seq = 0;
  item() { live.fetch_add(1, std::memory_order_relaxed); }
  item(int i, int p, long s) : id(i), producer(p), seq(s) { live.fetch_add(1, std::memory_order_relaxed); }
  item(const item& o) : id(o.id), producer(o.producer), seq(o.seq) {
    live.fetch_add(1, std::memory_order_relaxed);
  }
  item(item&& o) noexcept : producer(o.producer), seq(o.seq) {
    moved_out_of(o.id);
    id = o.id;
    live.fetch_add(1, std::memory_order_relaxed);
  }
  item& operator=(const item& o) {
    id = o.id;
    producer = o.producer;
    seq = o.seq;
    return *this;
  }
  item& operator=(item&& o) noexcept {
    moved_out_of(o.id);
    id = o.id;
    producer = o.producer;
    seq = o.seq;
    return *this;
  }
  ~item() { live.fetch_sub(1, std::memory_order_relaxed); }
};

// The empty answers of a linearisable queue: if c pushes had finished
// before a pop began, a pop that answers empty must be linearised after c
// elements were taken, each by a pop other than itself that began before
// it returned and did not answer empty before it began. The count of such
// pops is an upper bound read after the answer, so the check can miss a
// wrong answer but never reports a right one.
struct empty_check {
  std::atomic<long> pushes_done{0};
  std::atomic<long> pops_started{0};
  std::atomic<long> empties_done{0};
  std::atomic<long> violations{0};

  template <class Pop>
  bool pop(Pop&& do_pop) {
    const long empties_before = empties_done.load(std::memory_order_seq_cst);
    const long done_before = pushes_done.load(std::memory_order_seq_cst);
    pops_started.fetch_add(1, std::memory_order_seq_cst);
    const bool got = do_pop();
    if (!got) {
      const long others = pops_started.load(std::memory_order_seq_cst) - 1 - empties_before;
      if (done_before > others) violations.fetch_add(1);
      empties_done.fetch_add(1, std::memory_order_seq_cst);
    }
    return got;
  }
  void pushed(long n = 1) { pushes_done.fetch_add(n, std::memory_order_seq_cst); }
};

}  // namespace lemma
