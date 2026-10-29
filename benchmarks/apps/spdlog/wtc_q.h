// SPDX-License-Identifier: MIT
// Part of the WTC port of spdlog (benchmarks/apps/spdlog): the thread
// pool's queue operations over wtc::queue. Installed by port.py as
// spdlog/details/wtc_q.h in the port's include tree.
//
// The pool's queue is a wtc::queue<async_msg> member of the pool itself,
// built where the original's queue was, so that the compiler sees it and
// the loop that starts the workers in one constructor and can choose the
// tag when the pool is built from the number of workers: the logging
// threads come through the logger's virtual sink_it_, which is any number,
// and the workers are one per iteration of the loop over threads_n, so the
// queue is single-consumer when threads_n is one and general-purpose
// otherwise. With SPDLOG_WTC_HAND_MPSC the tag is declared by hand, which
// is what an expert who knew the pool had one worker would write, and what
// the compiler checks.
//
// What differs from the original queue: wtc::queue is unbounded, so the
// bound is kept beside it in a wtc_bound, as the pushes minus the pops; a
// push that would exceed it waits (block) or is dropped (discard_new), and
// a producer reads the pops' counter once per 64 of its own pushes, so the
// bound holds to within that. overrun_oldest cannot drop the oldest element
// of a queue its consumer owns, so it pushes without waiting and counts the
// overrun. A pop spins, then yields, instead of sleeping on a condition
// variable: a worker with nothing to do costs a core until it has, which
// is the regime every variant is measured in, and what a latency-sensitive
// logger does anyway.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>

#include "queue.hpp"

namespace spdlog {
namespace details {

#if defined(SPDLOG_WTC_HAND_LANED)
// An experiment, not a substitution: the laned queue (shim/topology.hpp,
// runtime/laned_mpsc.hpp) keeps each logging thread's order and none
// between them, and spdlog observes that order. A flush or the pool's
// terminate message, pushed by a thread after it has joined the logging
// threads, may come out before their messages: flush then returns early
// and shutdown drops what is left. This build measures the ceiling of
// relaxing the order, with bench.cpp waiting for the queue to drain
// before it flushes (SPDLOG_BENCH_DRAIN).
#define SPDLOG_WTC_TAG wtc::topology::mpsc_laned
#elif defined(SPDLOG_WTC_HAND_MPSC)
#define SPDLOG_WTC_TAG wtc::topology::mpsc
#else
#define SPDLOG_WTC_TAG
#endif

struct wtc_bound {
    explicit wtc_bound(size_t max_items) : max_items(max_items) {}
    const size_t max_items;
    alignas(64) std::atomic<size_t> pushed{0};
    alignas(64) std::atomic<size_t> popped{0};
    alignas(64) std::atomic<size_t> overrun{0};
    std::atomic<size_t> discard{0};

    size_t size() const {
        const size_t a = pushed.load(std::memory_order_relaxed);
        const size_t b = popped.load(std::memory_order_relaxed);
        return a > b ? a - b : 0;
    }
    // Whether this thread's next push would exceed the bound, decided from
    // the counters once per 64 of its pushes and remembered between.
    bool over() {
        thread_local unsigned since = 0;
        thread_local bool over = false;
        if (since++ % 64 == 0 || over)
            over = pushed.load(std::memory_order_relaxed) -
                       popped.load(std::memory_order_acquire) >= max_items;
        return over;
    }
};

inline void wtc_relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

template <typename T>
void wtc_push(wtc::queue<T> &q, wtc_bound &b, T &&item) {
    q.push(std::move(item));
    b.pushed.fetch_add(1, std::memory_order_relaxed);
}

template <typename T>
void wtc_enqueue(wtc::queue<T> &q, wtc_bound &b, T &&item) {
    while (b.over()) wtc_relax();
    wtc_push(q, b, std::move(item));
}

template <typename T>
void wtc_enqueue_nowait(wtc::queue<T> &q, wtc_bound &b, T &&item) {
    if (b.over()) ++b.overrun;
    wtc_push(q, b, std::move(item));
}

template <typename T>
void wtc_enqueue_if_have_room(wtc::queue<T> &q, wtc_bound &b, T &&item) {
    if (b.over()) {
        ++b.discard;
        return;
    }
    wtc_push(q, b, std::move(item));
}

template <typename T>
void wtc_dequeue(wtc::queue<T> &q, wtc_bound &b, T &out) {
    for (unsigned spins = 0; !q.try_pop(out); ++spins) {
        if (spins < 1024) wtc_relax();
        else std::this_thread::yield();
    }
    b.popped.fetch_add(1, std::memory_order_release);
}

template <typename T>
bool wtc_dequeue_for(wtc::queue<T> &q, wtc_bound &b, T &out, std::chrono::milliseconds wait) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (unsigned spins = 0;; ++spins) {
        if (q.try_pop(out)) {
            b.popped.fetch_add(1, std::memory_order_release);
            return true;
        }
        if ((spins & 255) == 255 && std::chrono::steady_clock::now() >= deadline) return false;
        if (spins < 1024) wtc_relax();
        else std::this_thread::yield();
    }
}

}  // namespace details
}  // namespace spdlog
