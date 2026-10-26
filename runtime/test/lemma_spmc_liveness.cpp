// Section 7 of docs/correctness.md claims that a substitution never
// introduces a wait the general-purpose queue would not also have had.
// For spmc it does: the consumers take turns under a spin lock, and the
// shim moves the element out (or builds the shared_ptr it returns) inside
// that turn, so the element type's move constructor runs while every other
// consumer waits. A program whose element's move takes a lock that another
// consumer holds while it pops then deadlocks under spmc and not under the
// general-purpose queue, whose consumers never wait for each other.
//
// The program: elements whose move constructor takes a mutex m. Consumer
// B holds m and pops; consumer A is already inside its pop, moving its
// element, and waits for m. Under the general-purpose queue B's pop takes
// the next cell without waiting for A, returns, and B lets m go. Under
// spmc B waits for A's turn, A waits for m: a deadlock. The test breaks it
// with a timed wait on m and counts the timeouts.
//
// Run with the tag as its argument: "generic" must report no timeout;
// "spmc" reports one, and is registered as expected to fail until the shim
// or the substitution changes (see the note in docs/correctness.md).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "../../shim/queue.hpp"

namespace {

std::timed_mutex m;
std::atomic<int> timeouts{0};
std::atomic<bool> a_in_move{false};
thread_local bool t_armed = false;

struct element {
  int v = 0;
  element() = default;
  explicit element(int x) : v(x) {}
  element(const element& o) : v(o.v) {}
  element(element&& o) noexcept : v(o.v) { take_m(); }
  element& operator=(const element& o) { v = o.v; return *this; }
  element& operator=(element&& o) noexcept { v = o.v; take_m(); return *this; }

  // The move a consumer makes takes m, the way an element that registers
  // itself somewhere would.
  static void take_m() {
    if (!t_armed) return;
    t_armed = false;
    a_in_move.store(true);
    if (m.try_lock_for(std::chrono::milliseconds(1500))) m.unlock();
    else timeouts.fetch_add(1);
  }
};

}  // namespace

int main(int argc, char** argv) {
  const bool spmc = argc > 1 && std::strcmp(argv[1], "spmc") == 0;
  {
    wtc::queue<element> q(spmc ? wtc::topology::spmc : wtc::topology::generic);
    q.push(element(1));  // one producer: this thread
    q.push(element(2));

    m.lock();  // consumer B, this thread, holds m
    std::thread a([&] {
      t_armed = true;
      std::shared_ptr<element> got = q.try_pop();  // moves its element, waits for m
      (void)got;
    });
    while (!a_in_move.load()) std::this_thread::yield();
    std::shared_ptr<element> mine = q.try_pop();  // B pops while it holds m
    m.unlock();
    a.join();
    if (!mine || mine->v != 2) {
      std::fprintf(stderr, "lemma_spmc_liveness: B popped the wrong element\n");
      return 2;
    }
  }
  if (timeouts.load()) {
    std::fprintf(stderr,
                 "lemma_spmc_liveness (%s): a consumer waited for another consumer's move: "
                 "a deadlock the general-purpose queue does not have\n",
                 spmc ? "spmc" : "generic");
    return 1;
  }
  std::printf("lemma_spmc_liveness (%s): ok\n", spmc ? "spmc" : "generic");
  return 0;
}
