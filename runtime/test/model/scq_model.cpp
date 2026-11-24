// Model-checking configurations of the SCQ ring and the queue over it
// (runtime/index_ring.hpp), for GenMC ("Model checking for weakly
// consistent libraries", PLDI 2019; the tool's RC11 mode), run by run.sh
// in this directory. Each configuration is a few threads making a few
// operations on a ring of two or four indices; GenMC explores every
// execution the C++ memory model allows, up to the loop bound run.sh
// gives, and fails on a failed assertion or a data race on the cells.
//
//   ring_pair    two enqueuers of different indices, two dequeuers, one
//                dequeue each: no index twice, none made up, none lost
//   ring_fifo    one enqueuer of 0 then 1, one dequeuer of two: never 1
//                before 0
//   ring_empty   an enqueue, then a flag: a dequeue that sees the flag
//                does not answer empty
//   ring_wrap    two threads each enqueue an index, then twice dequeue
//                one and enqueue it back: the cycles wrap on a ring of
//                four entries; nothing lost or doubled
//   queue_plain  the plain queue of capacity 1: a producer of two, a
//                consumer of two; FIFO, the element's value (its cell
//                written and read without atomics), at most one queued,
//                and a push begun after a pop returned finds room
//   queue_exact  the same with the exact count
//   queue_mpsc   two producers, one consumer of two: every element once
//
// Compiled with CONFIG set to one of the above (run.sh does).
#include <assert.h>
#include <pthread.h>
#include <stdint.h>

#include <atomic>
#include <new>

#include "../../index_ring.hpp"

using ring = wtc::rt::scq_ring<>;

alignas(64) static unsigned char g_storage[sizeof(wtc::rt::index_queue<int>) + sizeof(ring)];

#define ring_pair 1
#define ring_fifo 2
#define ring_empty 3
#define ring_wrap 4
#define queue_plain 5
#define queue_exact 6
#define queue_mpsc 7

#if CONFIG == ring_pair || CONFIG == ring_fifo || CONFIG == ring_empty || CONFIG == ring_wrap
static ring* r;
#else
static wtc::rt::index_queue<int>* q;
#endif

static int got[4] = {-2, -2, -2, -2};
static std::atomic<int> flag{0};

#if CONFIG == ring_pair
static void* enq0(void*) { r->enqueue(0); return nullptr; }
static void* enq1(void*) { r->enqueue(1); return nullptr; }
static void* deq0(void*) { std::uint32_t i; got[0] = r->dequeue(i) ? int(i) : -1; return nullptr; }
static void* deq1(void*) { std::uint32_t i; got[1] = r->dequeue(i) ? int(i) : -1; return nullptr; }
int main() {
  r = ::new (g_storage) ring(1, 0);
  pthread_t a, b, c, d;
  pthread_create(&a, nullptr, enq0, nullptr);
  pthread_create(&b, nullptr, enq1, nullptr);
  pthread_create(&c, nullptr, deq0, nullptr);
  pthread_create(&d, nullptr, deq1, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  pthread_join(c, nullptr);
  pthread_join(d, nullptr);
  int seen[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    assert(got[k] >= -1 && got[k] <= 1);
    if (got[k] >= 0) ++seen[got[k]];
  }
  std::uint32_t i;
  while (r->dequeue(i)) {
    assert(i <= 1);
    ++seen[i];
  }
  assert(seen[0] == 1 && seen[1] == 1);
  return 0;
}
#elif CONFIG == ring_fifo
static void* prod(void*) { r->enqueue(0); r->enqueue(1); return nullptr; }
static void* cons(void*) {
  std::uint32_t i;
  got[0] = r->dequeue(i) ? int(i) : -1;
  got[1] = r->dequeue(i) ? int(i) : -1;
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 0);
  pthread_t a, b;
  pthread_create(&a, nullptr, prod, nullptr);
  pthread_create(&b, nullptr, cons, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  // The dequeues' results, in order, are a prefix of 0, 1 with empty
  // answers only before 0 is taken or after 1 is.
  int next = 0;
  for (int k = 0; k < 2; ++k) {
    if (got[k] == -1) continue;
    assert(got[k] == next);
    ++next;
  }
  std::uint32_t i;
  while (r->dequeue(i)) {
    assert(int(i) == next);
    ++next;
  }
  assert(next == 2);
  return 0;
}
#elif CONFIG == ring_empty
static void* prod(void*) {
  r->enqueue(0);
  flag.store(1, std::memory_order_release);
  return nullptr;
}
static void* cons(void*) {
  if (flag.load(std::memory_order_acquire)) {
    std::uint32_t i;
    assert(r->dequeue(i) && i == 0);
  }
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 0);
  pthread_t a, b;
  pthread_create(&a, nullptr, prod, nullptr);
  pthread_create(&b, nullptr, cons, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  return 0;
}
#elif CONFIG == ring_wrap
static std::atomic<int> count[2];
static void* worker(void* arg) {
  const std::uint32_t mine = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(arg));
  r->enqueue(mine);
  count[mine].fetch_add(1);
  for (int k = 0; k < 2; ++k) {
    std::uint32_t i;
    // At most two indices exist and each thread holds none here, so the
    // ring holds both or one is in another thread's hands; a dequeue may
    // answer empty only in the latter case, and then this thread holds
    // nothing to put back.
    if (!r->dequeue(i)) continue;
    assert(i <= 1);
    count[i].fetch_sub(1);
    count[i].fetch_add(1);
    r->enqueue(i);
  }
  return nullptr;
}
int main() {
  r = ::new (g_storage) ring(1, 0);
  pthread_t a, b;
  pthread_create(&a, nullptr, worker, reinterpret_cast<void*>(std::uintptr_t{0}));
  pthread_create(&b, nullptr, worker, reinterpret_cast<void*>(std::uintptr_t{1}));
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  int seen[2] = {0, 0};
  std::uint32_t i;
  while (r->dequeue(i)) {
    assert(i <= 1);
    ++seen[i];
  }
  assert(seen[0] == 1 && seen[1] == 1);
  return 0;
}
#elif CONFIG == queue_plain || CONFIG == queue_exact
static int popped_before = 0;
static void* prod(void*) {
  got[2] = q->try_push(10) ? 1 : 0;
  popped_before = flag.load(std::memory_order_seq_cst);
  got[3] = q->try_push(20) ? 1 : 0;
  return nullptr;
}
static void* cons(void*) {
  int v = -1;
  got[0] = q->try_pop(v) ? v : -1;
  if (got[0] != -1) flag.store(1, std::memory_order_seq_cst);
  v = -1;
  got[1] = q->try_pop(v) ? v : -1;
  return nullptr;
}
int main() {
  q = ::new (g_storage) wtc::rt::index_queue<int>(1, CONFIG == queue_exact, 2);  // rings of order 1
  pthread_t a, b;
  pthread_create(&a, nullptr, prod, nullptr);
  pthread_create(&b, nullptr, cons, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  // The pushes that succeeded, in order, and the pops' results in order:
  // the pops' values are a prefix of the pushed ones.
  int pushed[2], np = 0;
  if (got[2]) pushed[np++] = 10;
  if (got[3]) pushed[np++] = 20;
  assert(np >= 1);  // the first push finds the queue empty
  int next = 0;
  for (int k = 0; k < 2; ++k) {
    if (got[k] == -1) continue;
    assert(next < np && got[k] == pushed[next]);
    ++next;
  }
  int v;
  int left = 0;
  while (q->try_pop(v)) {
    assert(next < np && v == pushed[next]);
    ++next;
    ++left;
  }
  assert(next == np);
  assert(left <= 1);  // never more than the capacity queued
  // A push that began after the only pop returned finds the room that
  // pop made: the pop gave its count back, and its cell, before it
  // returned.
  if (popped_before) assert(got[3]);
  return 0;
}
#elif CONFIG == queue_mpsc
static void* prod(void* arg) {
  const int v = static_cast<int>(reinterpret_cast<std::uintptr_t>(arg));
  assert(q->try_push(v));
  return nullptr;
}
static void* cons(void*) {
  int v = -1;
  got[0] = q->try_pop(v) ? v : -1;
  v = -1;
  got[1] = q->try_pop(v) ? v : -1;
  return nullptr;
}
int main() {
  q = ::new (g_storage) wtc::rt::index_queue<int>(2, false, 3);
  pthread_t a, b, c;
  pthread_create(&a, nullptr, prod, reinterpret_cast<void*>(std::uintptr_t{1}));
  pthread_create(&b, nullptr, prod, reinterpret_cast<void*>(std::uintptr_t{2}));
  pthread_create(&c, nullptr, cons, nullptr);
  pthread_join(a, nullptr);
  pthread_join(b, nullptr);
  pthread_join(c, nullptr);
  int seen[3] = {0, 0, 0};
  for (int k = 0; k < 2; ++k)
    if (got[k] != -1) {
      assert(got[k] == 1 || got[k] == 2);
      ++seen[got[k]];
    }
  int v;
  while (q->try_pop(v)) {
    assert(v == 1 || v == 2);
    ++seen[v];
  }
  assert(seen[1] == 1 && seen[2] == 1);
  return 0;
}
#else
#error "CONFIG: one of the configurations listed at the top"
#endif
