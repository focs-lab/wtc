// SPDX-License-Identifier: MIT
// Part of the WTC port of BS::thread_pool (benchmarks/apps/bs-thread-pool):
// the pool's task queue operations over wtc::queue, and how the control
// waits. Installed by port.py beside the altered BS_thread_pool.hpp, which
// includes it before its namespace opens.
//
// The pool's queue is a wtc::queue<task_t> member of the pool, so that the
// compiler sees the queue, its producers (whoever calls detach_task, or
// submit_task and the bulk submissions through it) and its consumers (the
// pool's workers) and can choose its implementation. With
// BS_WTC_HAND_SPMC the tag is declared by hand: one producer and any
// number of consumers, which is what an expert who knew that one thread
// submits would write, and what the compiler checks.
//
// The original keeps the queue, the count of running tasks and the flags
// under one mutex. The port keeps, beside the queue, what wait() and the
// counts need: the tasks submitted and not finished (queued or running,
// plus each worker until its initialisation is done, as the original
// counts them), the threads in wait(), the stop, and a flag per worker
// saying whether it runs a task. What differs from the original: a worker
// with nothing to do spins, then yields, instead of sleeping on a condition
// variable, so an idle worker costs a core; get_tasks_queued(),
// get_tasks_running() and get_tasks_total() are snapshots of counters
// rather than of one locked state, so the first two may disagree for an
// instant with the third; and the pool with priorities or pausing is not
// ported (a static_assert in the class says so).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>

#if defined(__cpp_lib_jthread)
#error "the WTC port builds BS::thread_pool as C++17 (std::thread and its stop flag)"
#endif

namespace BS {
namespace wtc_port {

inline void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// One step of waiting: a pause for the first 1024, a yield after.
inline void backoff(unsigned& spins) {
    if (spins++ < 1024) relax();
    else std::this_thread::yield();
}

}  // namespace wtc_port
}  // namespace BS

#ifndef BS_WTC_CONTROL

#include "queue.hpp"

#ifdef BS_WTC_HAND_SPMC
#define BS_WTC_TAG wtc::topology::spmc
#else
#define BS_WTC_TAG
#endif

namespace BS {
namespace wtc_port {

struct counts {
    // Submitted and not finished; a submitter and every worker write it.
    alignas(64) std::atomic<std::size_t> unfinished{0};
    // Read on every pop and every finished task, written rarely.
    alignas(64) std::atomic<unsigned> waiting{0};
    std::atomic<bool> stop{false};
};

struct alignas(64) busy_flag {
    std::atomic<bool> v{false};
};

// A task submitted: counted before it is pushed, so that no wait() can
// find the pool idle while the task is on its way.
template <class T>
void push(wtc::queue<T>& q, counts& c, T&& task) {
    c.unfinished.fetch_add(1, std::memory_order_seq_cst);
    q.push(std::move(task));
}

// A worker's pop: the oldest task moved into out, waiting for one; false
// once the pool is stopping, which is the original's condition for a
// worker to leave (whatever is still queued then, as in the original, is
// not run; the destructor waits for the tasks before it stops the pool).
template <class T>
bool pop(wtc::queue<T>& q, const counts& c, T& out) {
    for (unsigned spins = 0;;) {
        if (c.stop.load(std::memory_order_acquire)) return false;
        if (q.try_pop(out)) return true;
        backoff(spins);
    }
}

// A task, or a worker's initialisation, has finished. The one that leaves
// nothing unfinished wakes the threads in wait(): the decrement and the
// waiter's registration are both sequentially consistent, so either this
// thread sees the waiter or the waiter sees nothing unfinished, and the
// notification is sent under the mutex the waiter holds until it sleeps.
inline void finished(counts& c, std::mutex& m, std::condition_variable& cv) {
    if (c.unfinished.fetch_sub(1, std::memory_order_seq_cst) == 1 &&
        c.waiting.load(std::memory_order_seq_cst) != 0) {
        const std::scoped_lock lock(m);
        cv.notify_all();
    }
}

// wait()'s condition: nothing queued, nothing running.
inline bool idle(const counts& c) {
    return c.unfinished.load(std::memory_order_seq_cst) == 0;
}

inline std::size_t running(const busy_flag* busy, std::size_t n) {
    std::size_t k = 0;
    for (std::size_t i = 0; i < n; ++i) k += busy[i].v.load(std::memory_order_relaxed);
    return k;
}

inline std::size_t queued(const counts& c, const busy_flag* busy, std::size_t n) {
    const std::size_t all = c.unfinished.load(std::memory_order_relaxed), run = running(busy, n);
    return all > run ? all - run : 0;
}

// purge(): every queued task dropped unrun, each then finished.
template <class T>
void purge(wtc::queue<T>& q, counts& c, std::mutex& m, std::condition_variable& cv) {
    T t;
    while (q.try_pop(t)) {
        t = T();
        finished(c, m, cv);
    }
}

}  // namespace wtc_port
}  // namespace BS

#else

namespace BS {
namespace wtc_port {

// The control's wait: the original's condition-variable wait, done by
// releasing the mutex and spinning, then yielding, on the count the pool
// publishes under the mutex until it is not zero, then taking the mutex
// and checking the predicate again.
template <class Lock, class Pred>
void spin_wait(Lock& lock, const std::atomic<std::size_t>& ready, Pred pred) {
    unsigned spins = 0;
    while (!pred()) {
        lock.unlock();
        while (ready.load(std::memory_order_acquire) == 0) backoff(spins);
        lock.lock();
    }
}

}  // namespace wtc_port
}  // namespace BS

#endif
