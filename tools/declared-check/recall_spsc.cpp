// A single-producer single-consumer queue of a library, used as declared
// (SHAPE_OK) or by a second thread on one side, started in one of the ways
// C++ programs start threads. benchmarks/scripts/declared_recall.sh builds
// it once per library and shape, runs the declared-topology check on it, and
// runs the same program under ThreadSanitizer, so the two answers can be
// set side by side.
//
//   LIB_RIGTORP  rigtorp::SPSCQueue          (benchmarks/third_party)
//   LIB_MOODY    moodycamel::ReaderWriterQueue (benchmarks/third_party)
//   LIB_BOOST    boost::lockfree::spsc_queue (a Boost checkout on -I)
//
//   SHAPE_OK            one producer thread, one consumer thread
//   SHAPE_P2_FUNCTION   a second producer, std::thread on a function
//   SHAPE_P2_LAMBDA     a second producer, std::thread on a lambda
//   SHAPE_P2_PTHREAD    a second producer, pthread_create
//   SHAPE_P2_LOOP       producers started in a loop of two
//   SHAPE_C2_FUNCTION   a second consumer, std::thread on a function
//   SHAPE_P2_MAIN       main pushes too, besides the producer thread
//
// At run time the consumers count what they take and the program exits 1
// when the total is not what was pushed (a lost or doubled element).
#include <pthread.h>
#include <atomic>
#include <cstdio>
#include <thread>

#if defined(LIB_RIGTORP)
#include "rigtorp/SPSCQueue.h"
static rigtorp::SPSCQueue<int> q(1024);
static bool push(int v) { return q.try_push(v); }
static bool pop(int &v) {
  int *p = q.front();
  if (!p) return false;
  v = *p;
  q.pop();
  return true;
}
#elif defined(LIB_MOODY)
#include "readerwriterqueue/readerwriterqueue.h"
static moodycamel::ReaderWriterQueue<int> q(1024);
static bool push(int v) { return q.try_enqueue(v); }
static bool pop(int &v) { return q.try_dequeue(v); }
#elif defined(LIB_BOOST)
#include <boost/lockfree/spsc_queue.hpp>
static boost::lockfree::spsc_queue<int> q(1024);
static bool push(int v) { return q.push(v); }
static bool pop(int &v) { return q.pop(v); }
#else
#error "define LIB_RIGTORP, LIB_MOODY or LIB_BOOST"
#endif

#ifndef N
#define N 200000
#endif

static std::atomic<long> taken{0}, pushed{0};
static std::atomic<int> producers_done{0};

static void produce() {
  for (int i = 0; i < N; ++i)
    if (push(i)) pushed.fetch_add(1, std::memory_order_relaxed);
  producers_done.fetch_add(1);
}

static void consume(int producers) {
  int v;
  for (;;) {
    if (pop(v)) {
      taken.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (producers_done.load() == producers) {
      while (pop(v)) taken.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

#if defined(SHAPE_P2_PTHREAD)
static void *produce_entry(void *) {
  produce();
  return nullptr;
}
#endif

int main() {
#if defined(SHAPE_OK)
  std::thread p(produce), c(consume, 1);
  p.join(); c.join();
#elif defined(SHAPE_P2_FUNCTION)
  std::thread p(produce), p2(produce), c(consume, 2);
  p.join(); p2.join(); c.join();
#elif defined(SHAPE_P2_LAMBDA)
  std::thread p(produce), p2([] { produce(); }), c(consume, 2);
  p.join(); p2.join(); c.join();
#elif defined(SHAPE_P2_PTHREAD)
  pthread_t p2;
  std::thread p(produce), c(consume, 2);
  pthread_create(&p2, nullptr, produce_entry, nullptr);
  p.join(); pthread_join(p2, nullptr); c.join();
#elif defined(SHAPE_P2_LOOP)
  std::thread ps[2];
  for (auto &t : ps) t = std::thread(produce);
  std::thread c(consume, 2);
  for (auto &t : ps) t.join();
  c.join();
#elif defined(SHAPE_C2_FUNCTION)
  std::thread p(produce), c(consume, 1), c2(consume, 1);
  p.join(); c.join(); c2.join();
#elif defined(SHAPE_P2_MAIN)
  std::thread p(produce), c(consume, 2);
  produce();
  p.join(); c.join();
#else
#error "define one SHAPE_*"
#endif
  long t = taken.load(), s = pushed.load();
  std::printf("pushed %ld taken %ld\n", s, t);
  return t == s ? 0 : 1;
}
