// What the first operation of a thread, and of the process, does beyond
// its algorithm. Progress is claimed of an operation's own steps; a first
// use that takes a lock, allocates or spins for a measurement makes the
// claim false once per thread or once per process, which a real-time or
// signal-handler path observes.
//
//   tick_rate  the first catch-up of the spsc ring's and of the
//              single-consumer queue's consumer in the process: no
//              static-initialisation guard and well under the 200 µs the
//              counter's rate takes to measure, which the queue's
//              constructor has done.
//   first_ops  the first push and pop of a fresh thread on each queue that
//              uses hazard pointers: no allocator call, no guard, no
//              thread-exit registration (the thread_local destructor that
//              reaches the dynamic linker's lock) and no mutex.
//   keys40     forty pthread keys created before any queue: the hazard
//              key is past glibc's first 32, key_allocates() says so, and
//              a fresh thread's first push makes the one allocation
//              hazard.hpp states (glibc's block for the key's value) and
//              nothing else; its next push makes none.
//   enomem     as keys40, with that allocation failing: the push still
//              succeeds, the failure is counted (unhooked()), the next
//              push sets the key, and the thread's exit gives the record
//              back for the next thread.
//
// The counts come from interposing the C allocator and from wrapping, at
// link time, __cxa_guard_acquire, __cxa_thread_atexit and
// pthread_mutex_lock. Built without sanitizers only: their own
// interceptors would be counted too, and an interposed allocator would
// replace theirs.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "segment_queue.hpp"
#include "spmc_queue.hpp"
#include "spsc_ring.hpp"

namespace {
thread_local bool t_on = false;
thread_local int t_guards = 0;
thread_local int t_allocs = 0;    // malloc, calloc, realloc and the aligned forms
thread_local int t_frees = 0;
thread_local int t_atexits = 0;
thread_local int t_locks = 0;
thread_local bool t_fail_calloc = false;
int failures = 0;

#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)
}  // namespace

// -Wl,--wrap=...: what this program's own code calls. A guarded static
// initialisation, a thread_local destructor's registration (the compiler
// emits the call at the variable's first use), and a mutex.
extern "C" int __real___cxa_guard_acquire(std::uint64_t*);
extern "C" int __wrap___cxa_guard_acquire(std::uint64_t* g) {
  if (t_on) ++t_guards;
  return __real___cxa_guard_acquire(g);
}
extern "C" int __real___cxa_thread_atexit(void (*)(void*), void*, void*);
extern "C" int __wrap___cxa_thread_atexit(void (*f)(void*), void* o, void* d) {
  if (t_on) ++t_atexits;
  return __real___cxa_thread_atexit(f, o, d);
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* m) {
  if (t_on) ++t_locks;
  return __real_pthread_mutex_lock(m);
}

// The C allocator, interposed: the C++ allocator and the C library's own
// internal calls (the calloc inside __cxa_thread_atexit_impl) reach these.
extern "C" void* __libc_malloc(std::size_t);
extern "C" void* __libc_calloc(std::size_t, std::size_t);
extern "C" void* __libc_realloc(void*, std::size_t);
extern "C" void* __libc_memalign(std::size_t, std::size_t);
extern "C" void __libc_free(void*);
extern "C" void* malloc(std::size_t n) {
  if (t_on) ++t_allocs;
  return __libc_malloc(n);
}
extern "C" void* calloc(std::size_t a, std::size_t b) {
  if (t_on) ++t_allocs;
  if (t_fail_calloc) return nullptr;
  return __libc_calloc(a, b);
}
extern "C" void* realloc(void* p, std::size_t n) {
  if (t_on) ++t_allocs;
  return __libc_realloc(p, n);
}
extern "C" void* aligned_alloc(std::size_t a, std::size_t n) {
  if (t_on) ++t_allocs;
  return __libc_memalign(a, n);
}
extern "C" void* memalign(std::size_t a, std::size_t n) {
  if (t_on) ++t_allocs;
  return __libc_memalign(a, n);
}
extern "C" int posix_memalign(void** out, std::size_t a, std::size_t n) {
  if (t_on) ++t_allocs;
  void* p = __libc_memalign(a, n);
  if (!p) return 12;  // ENOMEM
  *out = p;
  return 0;
}
extern "C" void free(void* p) {
  if (t_on && p) ++t_frees;
  __libc_free(p);
}

namespace {
using clk = std::chrono::steady_clock;

struct probe_result {
  int guards, allocs, frees, atexits, locks;
  double us;
  bool clean() const { return guards == 0 && allocs == 0 && frees == 0 && atexits == 0 && locks == 0; }
};

template <class F>
probe_result probe(const char* what, F&& f) {
  t_guards = t_allocs = t_frees = t_atexits = t_locks = 0;
  t_on = true;
  const auto t0 = clk::now();
  f();
  const auto t1 = clk::now();
  t_on = false;
  const probe_result r{t_guards, t_allocs, t_frees, t_atexits, t_locks,
                       std::chrono::duration<double, std::micro>(t1 - t0).count()};
  std::printf("%-48s guards %d allocs %d frees %d atexits %d locks %d %8.1f us\n", what, r.guards,
              r.allocs, r.frees, r.atexits, r.locks, r.us);
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
// Each probe in a thread of its own, so that each is that thread's first
// operation; the queues were built, and hold elements, before.
template <class F>
void fresh(const char* what, F&& f) {
  std::thread([&] { EXPECT(probe(what, f).clean()); }).join();
}

void first_ops() {
  wtc::rt::segment_queue<std::uint64_t, true> g;
  wtc::rt::segment_queue<std::uint64_t, false> m;
  wtc::rt::spmc_queue<std::uint64_t> s;
  for (std::uint64_t i = 0; i < 8; ++i) {
    g.push(i);
    m.push(i);
    s.push(i);
  }
  std::uint64_t v = 0;
  fresh("generic push, first in a fresh thread", [&] { g.push(std::uint64_t{100}); });
  fresh("generic pop, first in a fresh thread", [&] { EXPECT(g.try_pop(v)); });
  fresh("mpsc push, first in a fresh thread", [&] { m.push(std::uint64_t{100}); });
  // The single consumer: one thread for the whole run, as its contract says.
  std::thread([&] {
    EXPECT(probe("mpsc pop, first in its thread", [&] { EXPECT(m.try_pop(v)); }).clean());
    for (int i = 0; i < 8; ++i) m.try_pop(v);
  }).join();
  fresh("spmc pop, first in a fresh thread", [&] { EXPECT(s.try_pop(v)); });
  fresh("spmc pop, first in another fresh thread", [&] { EXPECT(s.try_pop(v)); });
}

void make_keys() {
  pthread_key_t k;
  for (int i = 0; i < 40; ++i) EXPECT(pthread_key_create(&k, nullptr) == 0);
}

// The single-consumer queue, whose pushes past its second cell allocate
// nothing until a segment fills; the consumer is the main thread.
using mpsc_queue = wtc::rt::segment_queue<std::uint64_t, false>;

void keys40() {
  make_keys();
  mpsc_queue q;
  q.push(std::uint64_t{0});
  q.push(std::uint64_t{1});  // the push that links the next segment
  EXPECT(wtc::rt::hazard::key_allocates());
  for (int k = 0; k < 2; ++k)
    std::thread([&] {
      const probe_result a = probe("mpsc push, first in a fresh thread, key past 32", [&] { q.push(std::uint64_t{2}); });
      EXPECT(a.allocs == 1 && a.guards == 0 && a.atexits == 0 && a.locks == 0 && a.frees == 0);
      EXPECT(probe("mpsc push, second in that thread", [&] { q.push(std::uint64_t{3}); }).clean());
    }).join();
  EXPECT(wtc::rt::hazard::unhooked() == 0);
}

void enomem() {
  make_keys();
  mpsc_queue q;
  q.push(std::uint64_t{0});
  q.push(std::uint64_t{1});
  std::thread([&] {
    t_fail_calloc = true;
    q.push(std::uint64_t{2});
    t_fail_calloc = false;
    const std::uint64_t failed = wtc::rt::hazard::unhooked();
    EXPECT(failed == 1);
    q.push(std::uint64_t{3});  // sets the key this time
    EXPECT(wtc::rt::hazard::unhooked() == failed);
  }).join();
  const unsigned made = wtc::rt::hazard::records_made();
  std::thread([&] { q.push(std::uint64_t{4}); }).join();
  std::uint64_t v = 0, n = 0;
  while (q.try_pop(v)) EXPECT(v == n++);
  EXPECT(n == 5);
  std::printf("enomem: a failed key counted (%llu), the record given back at exit: records %u, then %u\n",
              (unsigned long long)wtc::rt::hazard::unhooked(), made, wtc::rt::hazard::records_made());
  EXPECT(wtc::rt::hazard::records_made() == made);
}
}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "tick_rate";
  if (!std::strcmp(mode, "tick_rate")) tick_rate();
  else if (!std::strcmp(mode, "first_ops")) first_ops();
  else if (!std::strcmp(mode, "keys40")) keys40();
  else if (!std::strcmp(mode, "enomem")) enomem();
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
