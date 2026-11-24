// Model-checking configurations of the wait-free ring (index_ring.hpp,
// wcq_ring) for GenMC, run by run.sh in this directory.
//
// GenMC models memory values of up to 64 bits, so the CAS2 of 64-bit
// pairs is checked here on pairs of 32-bit halves packed into one 64-bit
// word (packed_words below): the ring's code is the same template, with
// counters and cycles of 32 bits, which a run of a few operations never
// wraps. The halves' own operations (a fetch-and-add of the counter, a
// compare-and-swap or OR of the entry) are 64-bit operations on the word
// that leave the other half as it is.
//
// The ring is built with no patience for enqueues, one try for dequeues
// and help on every operation (PATIENCE_ENQ, PATIENCE_DEQ and DELAY, 0, 1
// and 1 by default), so that the slow path and the helping are what is
// explored.
//
//   wcq_pair   two threads enqueue different indices, a third dequeues
//              once: none lost, none twice, none made up
//   wcq_wrap   two threads each enqueue an index, then dequeue one and
//              enqueue it back: nothing lost or doubled
//   wcq_fifo   one thread enqueues 0 then 1, another dequeues twice:
//              never 1 before 0
//   wcq_empty  an enqueue, then a flag: a dequeue that sees the flag does
//              not answer empty
//   wcq_enq2   two threads each enqueue an index, both on the slow path
//              and each helping the other's request: each index once
//   wcq_enqdeq a ring that held index 0 and gave it to a dequeue (so
//              that the threshold lets the next dequeue try); then one
//              thread enqueues 1 while another dequeues once, which can
//              meet the enqueue's position taken and not yet produced and
//              go to its slow path, both helping each other: 1 once,
//              taken by the dequeue or left in the ring
//
// The ring holds two indices (four entries) and has a state per thread
// (STATES, 2 by default, 3 for wcq_pair): the main thread drains with
// state 0 after the others have exited, as a thread that takes over a
// slot does.
#include <assert.h>
#include <pthread.h>
#include <stdint.h>

#include <atomic>
#include <new>

#define WTC_RT_MODEL 1
#include "../../index_ring.hpp"

#ifndef PATIENCE_ENQ
#define PATIENCE_ENQ 0
#endif
#ifndef PATIENCE_DEQ
#define PATIENCE_DEQ 1
#endif
#ifndef DELAY
#define DELAY 1
#endif

static thread_local unsigned t_id = 0;

unsigned wtc::rt::thread_slots::self(bool& took_over) {
  took_over = false;
  return t_id;
}

struct packed_words {
  using word = std::uint32_t;
  using sword = std::int32_t;
  static constexpr bool kNeedsCas2 = false;
  struct pair {
    std::atomic<std::uint64_t> v;
  };
  static constexpr std::uint64_t kHi = 0xffffffff00000000ull;
  static std::uint64_t join(word lo, word hi) { return std::uint64_t{hi} << 32 | lo; }
  static void init(pair& p, word lo, word hi) { ::new (&p.v) std::atomic<std::uint64_t>(join(lo, hi)); }
  static word lo(pair& p) { return static_cast<word>(p.v.load(std::memory_order_seq_cst)); }
  static bool cas_lo(pair& p, word& expected, word desired) {
    std::uint64_t cur = p.v.load(std::memory_order_seq_cst);
    for (;;) {
      if (static_cast<word>(cur) != expected) {
        expected = static_cast<word>(cur);
        return false;
      }
      if (p.v.compare_exchange_strong(cur, (cur & kHi) | desired, std::memory_order_seq_cst)) return true;
    }
  }
  static word faa_lo(pair& p, word v) { return static_cast<word>(p.v.fetch_add(v, std::memory_order_seq_cst)); }
  static void or_lo(pair& p, word v) { p.v.fetch_or(v, std::memory_order_seq_cst); }
  static void load(pair& p, word& lo, word& hi) {
    const std::uint64_t v = p.v.load(std::memory_order_seq_cst);
    lo = static_cast<word>(v);
    hi = static_cast<word>(v >> 32);
  }
  static bool cas(pair& p, word& lo, word& hi, word new_lo, word new_hi) {
    std::uint64_t e = join(lo, hi);
    if (p.v.compare_exchange_strong(e, join(new_lo, new_hi), std::memory_order_seq_cst)) return true;
    lo = static_cast<word>(e);
    hi = static_cast<word>(e >> 32);
    return false;
  }
};

using ring = wtc::rt::wcq_ring<wtc::rt::no_ring_hooks, packed_words, wtc::rt::thread_slots, PATIENCE_ENQ,
                               PATIENCE_DEQ, DELAY>;

alignas(64) static unsigned char g_storage[sizeof(ring)];
static ring* r;
static int got[4] = {-2, -2, -2, -2};
static std::atomic<int> flag{0};

#define wcq_pair 1
#define wcq_wrap 2
#define wcq_fifo 3
#define wcq_empty 4
#define wcq_enq2 5
#define wcq_enqdeq 6
#ifndef STATES
#if CONFIG == wcq_pair
#define STATES 3
#else
#define STATES 2
#endif
#endif

static void drain_and_count(int* seen) {
  t_id = 0;
  std::uint32_t i;
  while (r->dequeue(i)) {
    assert(i <= 1);
    ++seen[i];
  }
}

#if CONFIG == wcq_pair
static void* enq(void* arg) {
  t_id = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(arg));
  r->enqueue(t_id);
  return nullptr;
}
static void* deq(void*) {
  t_id = 2;
  std::uint32_t i;
  got[0] = r->dequeue(i) ? int(i) : -1;
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 0, STATES);
  pthread_t a, b, c;
  pthread_create(&a, nullptr, enq, reinterpret_cast<void*>(std::uintptr_t{0}));
  pthread_create(&b, nullptr, enq, reinterpret_cast<void*>(std::uintptr_t{1}));
  pthread_create(&c, nullptr, deq, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  pthread_join(c, nullptr);
  int seen[2] = {0, 0};
  if (got[0] >= 0) ++seen[got[0]];
  drain_and_count(seen);
  assert(seen[0] == 1 && seen[1] == 1);
  return 0;
}
#elif CONFIG == wcq_wrap
static void* worker(void* arg) {
  t_id = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(arg));
  r->enqueue(t_id);
  std::uint32_t i;
  if (r->dequeue(i)) {
    assert(i <= 1);
    r->enqueue(i);
  }
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 0, STATES);
  pthread_t a, b;
  pthread_create(&a, nullptr, worker, reinterpret_cast<void*>(std::uintptr_t{0}));
  pthread_create(&b, nullptr, worker, reinterpret_cast<void*>(std::uintptr_t{1}));
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  int seen[2] = {0, 0};
  drain_and_count(seen);
  assert(seen[0] == 1 && seen[1] == 1);
  return 0;
}
#elif CONFIG == wcq_fifo
static void* prod(void*) {
  t_id = 0;
  r->enqueue(0);
  r->enqueue(1);
  return nullptr;
}
static void* cons(void*) {
  t_id = 1;
  std::uint32_t i;
  got[0] = r->dequeue(i) ? int(i) : -1;
  got[1] = r->dequeue(i) ? int(i) : -1;
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 0, STATES);
  pthread_t a, b;
  pthread_create(&a, nullptr, prod, nullptr);
  pthread_create(&b, nullptr, cons, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  int next = 0;
  for (int k = 0; k < 2; ++k) {
    if (got[k] == -1) continue;
    assert(got[k] == next);
    ++next;
  }
  t_id = 0;
  std::uint32_t i;
  while (r->dequeue(i)) {
    assert(int(i) == next);
    ++next;
  }
  assert(next == 2);
  return 0;
}
#elif CONFIG == wcq_empty
static void* prod(void*) {
  t_id = 0;
  r->enqueue(0);
  flag.store(1, std::memory_order_release);
  return nullptr;
}
static void* cons(void*) {
  t_id = 1;
  if (flag.load(std::memory_order_acquire)) {
    std::uint32_t i;
    assert(r->dequeue(i) && i == 0);
  }
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 0, STATES);
  pthread_t a, b;
  pthread_create(&a, nullptr, prod, nullptr);
  pthread_create(&b, nullptr, cons, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  return 0;
}
#elif CONFIG == wcq_enq2
static void* enq(void* arg) {
  t_id = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(arg));
  r->enqueue(t_id);
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 0, STATES);
  pthread_t a, b;
  pthread_create(&a, nullptr, enq, reinterpret_cast<void*>(std::uintptr_t{0}));
  pthread_create(&b, nullptr, enq, reinterpret_cast<void*>(std::uintptr_t{1}));
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  int seen[2] = {0, 0};
  drain_and_count(seen);
  assert(seen[0] == 1 && seen[1] == 1);
  return 0;
}
#elif CONFIG == wcq_enqdeq
static void* enq(void*) {
  t_id = 0;
  r->enqueue(1);
  return nullptr;
}
static void* deq(void*) {
  t_id = 1;
  std::uint32_t i;
  got[0] = r->dequeue(i) ? int(i) : -1;
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 1, STATES);
  t_id = 0;
  std::uint32_t first;
  assert(r->dequeue(first) && first == 0);
  pthread_t a, b;
  pthread_create(&a, nullptr, enq, nullptr);
  pthread_create(&b, nullptr, deq, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  int seen[2] = {0, 0};
  if (got[0] >= 0) ++seen[got[0]];
  drain_and_count(seen);
  assert(seen[0] == 0 && seen[1] == 1);
  return 0;
}
#else
#error "CONFIG: one of the configurations listed at the top"
#endif
