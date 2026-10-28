// SPDX-License-Identifier: Zlib
// Part of the WTC port of ThreadPool (benchmarks/apps/threadpool): how a
// worker takes a task from the pool's wtc::queue, and how the control
// waits. Installed by port.py beside the altered ThreadPool.h.
//
// The pool's queue is a wtc::queue<std::function<void()>> member of the
// pool, built by the pool's constructor before the loop that starts the
// workers, so that the compiler sees the queue, its producers (whoever
// calls enqueue) and its consumers (one per iteration of that loop) and can
// choose the implementation when the pool is built. With
// THREADPOOL_WTC_HAND_SPMC the tag is declared by hand: one producer and
// any number of consumers, which is what an expert who knew that one thread
// submits would write, and what the compiler checks.
//
// What differs from the original: a worker with nothing to do spins, then
// yields, instead of sleeping on a condition variable, so an idle worker
// costs a core; and enqueue takes no mutex, so it no longer serialises
// against the destructor: enqueue concurrent with the pool's destruction
// is a use after the end of the pool's lifetime in both, and only the
// original turns it into an exception.
#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>

inline void wtc_pool_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// One step of waiting: a pause for the first 1024, a yield after.
inline void wtc_pool_backoff(unsigned& spins) {
    if (spins++ < 1024) wtc_pool_relax();
    else std::this_thread::yield();
}

#ifndef THREADPOOL_WTC_CONTROL

#include "queue.hpp"

#ifdef THREADPOOL_WTC_HAND_SPMC
#define THREADPOOL_WTC_TAG wtc::topology::spmc
#else
#define THREADPOOL_WTC_TAG
#endif

// A worker's pop: the oldest task moved into out, waiting for one; false
// once the pool is stopping and no task is left, which is the original's
// condition for a worker to return. Every push happens before the store
// that stops the pool (the destructor runs after the last enqueue), so a
// pop that fails after the stop has been seen means the queue is empty.
template <class T>
bool wtc_pool_pop(wtc::queue<T>& q, const std::atomic<bool>& stop, T& out) {
    for (unsigned spins = 0;;) {
        if (q.try_pop(out)) return true;
        if (stop.load(std::memory_order_acquire)) return q.try_pop(out);
        wtc_pool_backoff(spins);
    }
}

#else

// The control's wait: the original's condition-variable wait, done by
// releasing the mutex and spinning, then yielding, on the count the pool
// publishes under the mutex until it is not zero, then taking the mutex
// and checking the predicate again.
template <class Pred>
void wtc_spin_wait(std::unique_lock<std::mutex>& lock, const std::atomic<std::size_t>& ready, Pred pred) {
    unsigned spins = 0;
    while (!pred()) {
        lock.unlock();
        while (ready.load(std::memory_order_acquire) == 0) wtc_pool_backoff(spins);
        lock.lock();
    }
}

#endif
