// The hazard-pointer domain (hazard.hpp) under thread churn.
//
//   churn N   N threads, kWave at a time, each pushing and popping on a
//             general-purpose queue of small segments and pushing to a
//             single-consumer queue that the main thread drains: every
//             element arrives once, and the records are reused, so that no
//             more are ever made than threads held them at once.
//   late      threads whose last round of key destructors (glibc runs at
//             most four) pushes to and pops from queues, after the
//             hazard key's destructor gave their records back: those
//             operations borrow a record each and give it back, so fifty
//             such threads, one at a time, leave no record taken.
//   late_two  one such thread makes two operations in its last round, and
//             between them another thread takes a record and holds it:
//             the second operation borrows a record of its own, never the
//             one the first gave back and the other thread now holds.
//   fork      a fork while eight threads hold records: in the child, eight
//             new threads holding records at once take the records the
//             parent's threads held, and no more are made.
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "hazard.hpp"
#include "segment_queue.hpp"

namespace {
int failures = 0;

#define EXPECT(c)                                                            \
  do {                                                                       \
    if (!(c)) {                                                              \
      std::fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #c); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

constexpr unsigned kWave = 16;

void churn(long n) {
  // Eight cells a segment: segments are retired all the time, by threads
  // that then exit with some of them still in their records.
  wtc::rt::segment_queue<std::uint64_t, true, 8> g;
  wtc::rt::segment_queue<std::uint64_t, false, 8> m;
  std::atomic<long> popped{0};
  std::uint64_t sum_in = 0, sum_out = 0;
  long got = 0;
  for (long base = 0; base < n; base += kWave) {
    std::vector<std::thread> ts;
    // The wave's threads hold their records at once: each waits at the
    // start for the others to have taken theirs.
    const unsigned wave = static_cast<unsigned>(n - base < kWave ? n - base : kWave);
    std::atomic<unsigned> arrived{0};
    for (unsigned k = 0; k < wave; ++k) {
      const std::uint64_t id = static_cast<std::uint64_t>(base + k);
      sum_in += id;
      ts.emplace_back([&, id, wave] {
        g.push(id);
        arrived.fetch_add(1);
        while (arrived.load() < wave) std::this_thread::yield();
        for (int i = 1; i < 5; ++i) g.push(id);
        std::uint64_t v;
        for (int i = 0; i < 5; ++i)
          if (g.try_pop(v)) popped.fetch_add(1);
        m.push(id);
      });
    }
    for (auto& t : ts) t.join();
    std::uint64_t v;
    while (m.try_pop(v)) {
      sum_out += v;
      ++got;
    }
  }
  std::uint64_t v;
  while (g.try_pop(v)) popped.fetch_add(1);
  EXPECT(got == n);
  EXPECT(sum_out == sum_in);
  EXPECT(popped.load() == 5 * n);
  // The waves' threads and the main thread.
  const unsigned made = wtc::rt::hazard::records_made();
  std::printf("churn: %ld threads, %u records made\n", n, made);
  EXPECT(made >= kWave);
  EXPECT(made <= kWave + 1);
}
// A key whose destructor sets itself again, round after round, and makes
// its operations in the round asked for.
pthread_key_t g_late_key;
long g_late_round = 4;
wtc::rt::segment_queue<std::uint64_t, true, 8>* g_late_g = nullptr;
wtc::rt::segment_queue<std::uint64_t, false, 8>* g_late_m = nullptr;
void late_dtor(void* p) {
  const long round = reinterpret_cast<long>(p);
  if (round == g_late_round) {
    g_late_g->push(std::uint64_t{1});
    std::uint64_t v;
    g_late_g->try_pop(v);
    g_late_m->push(std::uint64_t{2});
  }
  if (round < 4) pthread_setspecific(g_late_key, reinterpret_cast<void*>(round + 1));
}

void late() {
  wtc::rt::segment_queue<std::uint64_t, true, 8> g;
  wtc::rt::segment_queue<std::uint64_t, false, 8> m;
  g_late_g = &g;
  g_late_m = &m;
  EXPECT(pthread_key_create(&g_late_key, late_dtor) == 0);
  for (int t = 0; t < 50; ++t)
    std::thread([] {
      g_late_g->push(std::uint64_t{0});
      std::uint64_t v;
      g_late_g->try_pop(v);
      pthread_setspecific(g_late_key, reinterpret_cast<void*>(1));
    }).join();
  std::uint64_t v;
  long got = 0;
  while (m.try_pop(v)) ++got;
  const unsigned made = wtc::rt::hazard::records_made();
  std::printf("late: 50 threads operating in their last key-destructor round: %ld pushes arrived, "
              "%u records made\n", got, made);
  EXPECT(got == 50);
  // Each thread's own record and the borrowed one, reused by the next
  // thread, and the main thread's.
  EXPECT(made <= 3);
}

// late_two: the exiting thread's two operations and the thread between them.
pthread_key_t g_late2_key;
wtc::rt::segment_queue<std::uint64_t, true, 8>* g_late2_q = nullptr;
std::atomic<int> g_late2_stage{0};
std::atomic<wtc::rt::hazard::record*> g_late2_held{nullptr}, g_late2_second{nullptr};
void late2_dtor(void* p) {
  const long round = reinterpret_cast<long>(p);
  if (round == 4) {
    g_late2_q->push(std::uint64_t{1});  // the first: borrows a record and gives it back
    g_late2_stage = 1;
    while (g_late2_stage.load() != 2) std::this_thread::yield();
    const wtc::rt::hazard::use second;  // the second
    g_late2_second = &static_cast<wtc::rt::hazard::record&>(second);
    g_late2_q->push(std::uint64_t{2});
  }
  if (round < 4) pthread_setspecific(g_late2_key, reinterpret_cast<void*>(round + 1));
}

void late_two() {
  wtc::rt::segment_queue<std::uint64_t, true, 8> q;  // the main thread takes no record
  g_late2_q = &q;
  EXPECT(pthread_key_create(&g_late2_key, late2_dtor) == 0);
  std::thread holder([] {
    while (g_late2_stage.load() != 1) std::this_thread::yield();
    g_late2_q->push(std::uint64_t{3});  // takes the idle record, the one the first gave back
    g_late2_held = &wtc::rt::hazard::mine();
    g_late2_stage = 2;
    while (g_late2_stage.load() != 3) std::this_thread::yield();
  });
  std::thread([] {
    g_late2_q->push(std::uint64_t{0});
    pthread_setspecific(g_late2_key, reinterpret_cast<void*>(1));
  }).join();
  g_late2_stage = 3;
  holder.join();
  std::printf("late_two: the second late operation's record %p, the other thread's %p\n",
              static_cast<void*>(g_late2_second.load()), static_cast<void*>(g_late2_held.load()));
  EXPECT(g_late2_second.load() != nullptr);
  EXPECT(g_late2_second.load() != g_late2_held.load());
  std::uint64_t v;
  int got = 0;
  while (q.try_pop(v)) ++got;
  EXPECT(got == 4);
}

void forked() {
  wtc::rt::segment_queue<std::uint64_t, false> q;
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::vector<std::thread> holders;
  for (int t = 0; t < 8; ++t)
    holders.emplace_back([&] {
      q.push(1);
      ready.fetch_add(1);
      while (!stop.load()) std::this_thread::yield();
    });
  while (ready.load() < 8) std::this_thread::yield();
  const unsigned made = wtc::rt::hazard::records_made();
  std::fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) {
    std::atomic<int> in{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t)
      ts.emplace_back([&] {
        q.push(2);
        in.fetch_add(1);
        while (in.load() < 8) std::this_thread::yield();
      });
    for (auto& t : ts) t.join();
    const unsigned now = wtc::rt::hazard::records_made();
    std::printf("fork: records %u before, %u after eight threads in the child\n", made, now);
    std::fflush(stdout);
    _exit(now == made ? 0 : 1);
  }
  int status = 0;
  EXPECT(waitpid(pid, &status, 0) == pid);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  stop = true;
  for (auto& t : holders) t.join();
}

}  // namespace

int main(int argc, char** argv) {
  const char* mode = argc > 1 ? argv[1] : "churn";
  const long n = argc > 2 ? std::atol(argv[2]) : 10000;
  if (!std::strcmp(mode, "churn")) churn(n);
  else if (!std::strcmp(mode, "fork")) forked();
  else if (!std::strcmp(mode, "late")) late();
  else if (!std::strcmp(mode, "late_two")) late_two();
  else {
    std::fprintf(stderr, "hazard_test: unknown mode %s\n", mode);
    return 2;
  }
  if (failures) {
    std::fprintf(stderr, "hazard_test %s: %d failure(s)\n", mode, failures);
    return 1;
  }
  std::printf("hazard_test %s: ok\n", mode);
  return 0;
}
