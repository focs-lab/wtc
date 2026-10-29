// wtc::rt::waiter: a consumer's wait for an element, shared by every queue
// implementation, with three policies: spin, park, spin_then_park.
//
// What it adds to a queue: a pop that waits (pop_wait), with or without a
// time limit, and close(), after which a waiting pop that finds the queue
// empty returns false instead of waiting on. It needs nothing of the queue
// but its non-blocking pop and a call to notify after every publish, so the
// same waiter serves the spsc ring, both forms of the segmented queue and
// the spmc queue, and changes none of them: try_pop stays exactly what it
// was.
//
// Why it is needed. Thread pools as written sleep on a condition variable
// and every push signals it. Measured on four of them (BS::thread_pool,
// ThreadPool (C++11), CTPL, libuv's pool), that signal, a futex wake of
// a sleeping worker, costs the submitter 2 to 5 µs a task at eight workers
// and is what limits them; a lock-free queue under the same signal barely
// helps (CTPL's boost::lockfree variant runs at 0.85-0.95 of the time of
// its std::queue variant). A pool's workers ported onto the shim used to
// spin and then yield instead, which removes the signal and costs a core
// per idle worker. A waiter that parks gets the condition variable's idle
// cost without its per-push cost: a push signals only when a consumer is
// actually asleep, and otherwise pays one fence and one load.
//
// The protocol is an eventcount (Folly's EventCount is the same idea). A
// consumer about to sleep first announces itself: it counts itself
// in waiters_ and reads the epoch, the futex word. Then it looks at the
// queue once more, and sleeps only if that look finds nothing, and only for
// as long as the epoch still has the value it read. A producer publishes
// its element, then looks at waiters_; only if someone is announced does it
// bump the epoch and wake a sleeper. The bump is what makes a sleep that
// starts after it return at once: the futex sleeps only while the word
// still holds the value the consumer read.
//
// The lost wake-up, and the two fences that rule it out. Each side writes
// one location and then reads the other's:
//
//   producer:  publish element (queue's atomics)   consumer:  waiters_ += 1
//              fence(seq_cst)              X                   fence(seq_cst)   Y
//              read waiters_                                   look at the queue
//
// Without the fences both reads may return the old value: each store can
// still sit in its core's store buffer when the other core reads (x86
// allows exactly this reordering of a store with a later load, and the C++
// model allows it for anything weaker than sequential consistency on both
// sides). The producer then sees nobody waiting and wakes no one, the
// consumer sees no element and sleeps, and the element waits for the next
// push, or for ever. With a sequentially consistent fence on each side the
// two fences are ordered in the single total order S, and either order
// rescues it: if X comes before Y, the consumer's look, after Y, observes
// the publish, which came before X ([atomics.order], fences ordered in S);
// if Y comes before X, the producer's read, after X, observes the increment,
// which came before Y, and the producer wakes it. The increment is relaxed
// and the publish whatever release the queue uses: the fences carry the
// ordering. The epoch is read after Y and before the look, so a wake-up
// that bumps it after the read makes the sleep return at once, and one that
// bumped it before the read was by a producer whose element the look sees
// (the read acquires the bump, which the producer made after publishing).
//
// close() is the same pattern with closed_ as the published location, and
// it wakes everyone unconditionally: it is rare, and its waiters must all
// see it.
//
// Lifetime. notify() runs after the element is visible: it reads the
// policy and waiters_ and may bump the epoch and wake. A consumer may by
// then have taken the element and returned. So the queue, and the waiter
// in it, may be destroyed only after every push has returned (its
// producers joined, or otherwise ordered before the destructor), not as
// soon as the last element has been popped. A one-shot reply queue on the
// requester's stack, destroyed as soon as the reply arrives, would let
// the replying thread write freed memory. The same rule holds for the
// queues themselves (their headers' "destructor runs after all of them
// have made their last call"), and in Folly's and moodycamel's queues.
//
// The cost to a producer is paid in full only by a queue whose policy can
// park: a spin queue's consumers never sleep, so its push only reads the
// policy, on a line nobody writes, and branches past the rest. A queue
// that can park pays, per publish (a push, or a
// batch's publish), one fence and one read of a line that only changes when
// a consumer goes to sleep or wakes; the futex wake, a system call of one
// to a few microseconds, only when a consumer sleeps. The fence is not
// cheap where a queue streams: it holds the producer until the store that
// published is done, and while a consumer polls, that store has to fetch
// the line back from the consumer's core first, which on a ring streaming
// small elements takes several times as long as the rest of the push. So
// the shim's default policy is spin, which adds only that read and branch
// to a push, and a
// policy that parks is for queues whose consumers wait for work that comes
// at intervals, such as a pool's tasks, where the fence is lost in the
// rest of a push.
//
// The spin budget is a time, not a count of iterations: a pause instruction
// takes from about ten cycles to over a hundred depending on the processor,
// so a count would mean a different wait on every host. It is kept in
// processor ticks, converted from nanoseconds with the rate stream_gate.hpp
// measures once per process. The default, kSpinNs, is about the cost of a
// park and its wake-up together, so a consumer that spins and then parks
// never pays more than twice what the better of the two choices would have
// cost it (the classic competitive argument for spin-then-block).
//
// Wake-ups. A push wakes one sleeper; a batch's publish wakes as many as it
// published. A sleeper that wakes and finds the queue already emptied by a
// consumer that never slept goes back to sleep, which costs a wake-up and
// loses nothing: the element went to a consumer. Every sleeper is counted in
// waiters_ from before its last look until after it has woken, so a publish
// while any consumer sleeps always sees it.
//
// The kernel: Linux's futex, private to the process, on the 32-bit epoch;
// with a time limit, the wait is relative and measured on the monotonic
// clock. Elsewhere, a mutex and a condition variable stand in for it.
// ThreadSanitizer does not see the futex, and needs not: every ordering the
// protocol relies on goes through the atomics above.
#pragma once

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <thread>

#if defined(__linux__)
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
// The system call itself, declared as glibc declares it, rather than by
// <unistd.h>: the shim is included by every program that uses a queue, and
// <unistd.h> would bring read, write, close, sleep and the rest into each
// of them, against declarations of their own.
#if defined(__GLIBC__)
extern "C" long syscall(long number, ...) noexcept;
#else
#include <unistd.h>
#endif
#else
#include <condition_variable>
#include <mutex>
#endif

#include "stream_gate.hpp"

#ifndef WTC_SPIN_NS
#define WTC_SPIN_NS 4000
#endif

#if defined(__SANITIZE_THREAD__)
#define WTC_RT_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define WTC_RT_TSAN 1
#endif
#endif

namespace wtc::rt {

// How a waiting pop waits. Chosen when the queue is built, like its
// topology.
//
//   spin            poll the queue, with a pause between looks, for the
//                   spin budget, then yield the processor between looks;
//                   never sleeps, so a push signals nothing
//   park            look once more after announcing itself, then sleep in
//                   the kernel until a push or close() wakes it
//   spin_then_park  spin for the budget, then park
//
// The underlying type is int for the same reason as the topology's: the
// compiler reads and writes it as a constant at the constructor call.
enum class wait_policy : int {
    spin = 0,
    park = 1,
    spin_then_park = 2,
};

class waiter {
public:
    static constexpr std::uint64_t kSpinNs = WTC_SPIN_NS;

    explicit waiter(wait_policy policy = wait_policy::spin, std::uint64_t spin_ns = kSpinNs)
        : policy_(policy), spin_ns_(spin_ns) {}

    waiter(const waiter&) = delete;
    waiter& operator=(const waiter&) = delete;

    wait_policy policy() const { return policy_; }

    // Waits until try_pop() returns true, and returns true; or returns false
    // once the waiter is closed and try_pop() still finds nothing. try_pop
    // is the queue's non-blocking pop, called by this thread only.
    template <class TryPop>
    bool wait(TryPop&& try_pop) {
        return wait_until(try_pop, /*limited=*/false, 0);
    }

    // The same with a time limit: false also when the limit passes with the
    // queue empty. A limit of zero or less looks once; one of more than
    // kForeverNs is no limit.
    template <class TryPop>
    bool wait_for(TryPop&& try_pop, std::chrono::nanoseconds limit) {
        if (try_pop()) return true;
        if (limit.count() <= 0) return false;
        if (static_cast<std::uint64_t>(limit.count()) > kForeverNs) return wait_until(try_pop, false, 0);
        const std::uint64_t until = ticks() + to_ticks(static_cast<std::uint64_t>(limit.count()));
        return wait_until(try_pop, /*limited=*/true, until);
    }

    // After every publish, with the number of elements it made visible: wakes
    // up to that many sleepers, if any are announced. Nothing at all for a
    // spin waiter. The fence pairs with the one in park(); see the comment
    // at the top.
    void notify(std::uint64_t published = 1) noexcept {
        if (policy_ == wait_policy::spin || published == 0) return;
#ifdef WTC_TEST_DROP_WAKEUPS
        return;  // runtime/test/waiter_test.cpp: the test that the test can fail
#endif
        fence();
        if (waiters_.load(std::memory_order_relaxed) == 0) return;
        wake(published > INT_MAX ? INT_MAX : static_cast<int>(published));
    }

    // From then on, a waiting pop that finds the queue empty returns false.
    // Every sleeper is woken. Elements pushed before close() are still
    // popped; whether one pushed concurrently with it is, depends on which
    // a consumer sees first. Any thread may call it, any number of times.
    void close() noexcept {
        closed_.store(true, std::memory_order_release);
        fence();
        wake(INT_MAX);
    }

    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    // Hints for tests: consumers announced now, and times a consumer has
    // gone to sleep.
    std::uint32_t waiting() const { return waiters_.load(std::memory_order_relaxed); }
    std::uint64_t parks() const { return parks_.load(std::memory_order_relaxed); }

private:
    // About three years: past it a limit is no limit, which keeps the
    // conversion to ticks below from overflowing.
    static constexpr std::uint64_t kForeverNs = std::uint64_t{100000000} * 1000000000;

    static bool passed(std::uint64_t until, std::uint64_t now) {
        return static_cast<std::int64_t>(until - now) <= 0;
    }

    // The ticks one nanosecond count, as stream_gate.hpp measures them.
    static std::uint64_t to_ticks(std::uint64_t ns) {
        const std::uint64_t per_ms = ticks_per_ms();
        // ns * per_ms / 10^6 without overflow for any limit under about
        // five hundred years at 10 GHz.
        return ns / 1000000 * per_ms + ns % 1000000 * per_ms / 1000000;
    }
    static std::uint64_t to_ns(std::uint64_t t) {
        const std::uint64_t per_ms = ticks_per_ms();
        return t / per_ms * 1000000 + t % per_ms * 1000000 / per_ms;
    }

    static void relax() {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    // A sequentially consistent fence. ThreadSanitizer does not model
    // fences, and GCC rejects them under it (-Wtsan); there a sequentially
    // consistent read-modify-write of a location of this object stands in,
    // which on x86 is the same full barrier.
    void fence() noexcept {
#ifdef WTC_RT_TSAN
        tsan_fence_.fetch_add(0, std::memory_order_seq_cst);
#else
        std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
    }

    template <class TryPop>
    bool wait_until(TryPop& try_pop, bool limited, std::uint64_t until) {
        // Spin: poll with a pause between looks for the budget, then, for a
        // spin waiter, yield between looks for as long as it takes.
        if (policy_ != wait_policy::park) {
            const std::uint64_t start = ticks();
            const std::uint64_t budget = to_ticks(spin_ns_);
            for (;;) {
                // closed first: a closed waiter's last look must come after
                // it has seen the close, so that it finds every element
                // pushed before close().
                const bool was_closed = closed();
                if (try_pop()) return true;
                if (was_closed) return false;
                const std::uint64_t now = ticks();
                if (limited && passed(until, now)) return false;
                if (now - start < budget) {
                    relax();
                } else if (policy_ == wait_policy::spin) {
                    std::this_thread::yield();
                } else {
                    break;
                }
            }
        }
        // Park: announce, look again, sleep while nothing has changed.
        for (;;) {
            const bool was_closed_before = closed();
            if (try_pop()) return true;
            if (was_closed_before) return false;
            std::uint64_t left_ns = 0;
            if (limited) {
                const std::uint64_t now = ticks();
                if (passed(until, now)) return false;
                left_ns = to_ns(until - now);
                if (left_ns == 0) left_ns = 1;
            }
            const std::uint32_t key = prepare();
            const bool was_closed = closed();
            if (try_pop()) {
                cancel();
                return true;
            }
            if (was_closed) {
                cancel();
                return false;
            }
            parks_.fetch_add(1, std::memory_order_relaxed);
            sleep(key, left_ns);
            cancel();
        }
    }

    // Counts this consumer among the waiters and returns the epoch, after
    // the fence that pairs with notify's.
    std::uint32_t prepare() noexcept {
        waiters_.fetch_add(1, std::memory_order_relaxed);
        fence();
        return epoch_.load(std::memory_order_acquire);
    }

    void cancel() noexcept { waiters_.fetch_sub(1, std::memory_order_relaxed); }

    // Sleeps while the epoch is still key, for up to left_ns when that is
    // not zero. Returns early on a wake-up, a signal or a spurious return;
    // the caller looks at the queue again in every case.
    void sleep(std::uint32_t key, std::uint64_t left_ns) noexcept {
        if (epoch_.load(std::memory_order_acquire) != key) return;
#if defined(__linux__)
        timespec ts{};
        timespec* tp = nullptr;
        if (left_ns) {
            ts.tv_sec = static_cast<time_t>(left_ns / 1000000000);
            ts.tv_nsec = static_cast<long>(left_ns % 1000000000);
            tp = &ts;
        }
        syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&epoch_), FUTEX_WAIT_PRIVATE, key, tp,
                nullptr, 0);
#else
        std::unique_lock<std::mutex> lk(m_);
        if (left_ns)
            cv_.wait_for(lk, std::chrono::nanoseconds(left_ns),
                         [&] { return epoch_.load(std::memory_order_acquire) != key; });
        else
            cv_.wait(lk, [&] { return epoch_.load(std::memory_order_acquire) != key; });
#endif
    }

    void wake(int n) noexcept {
        epoch_.fetch_add(1, std::memory_order_release);
#if defined(__linux__)
        syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&epoch_), FUTEX_WAKE_PRIVATE, n, nullptr,
                nullptr, 0);
#else
        { std::lock_guard<std::mutex> lk(m_); }
        if (n == 1) cv_.notify_one();
        else cv_.notify_all();
#endif
    }

    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t),
                  "the futex word must be a plain 32-bit word");

    // Read-only after construction.
    const wait_policy policy_;
    const std::uint64_t spin_ns_;
    // Read by every publish of a queue that can park; written when a
    // consumer announces itself or leaves.
    std::atomic<std::uint32_t> waiters_{0};
    // The futex word: bumped by every wake-up.
    std::atomic<std::uint32_t> epoch_{0};
    std::atomic<bool> closed_{false};
    std::atomic<std::uint64_t> parks_{0};
#ifdef WTC_RT_TSAN
    std::atomic<std::uint32_t> tsan_fence_{0};
#endif
#if !defined(__linux__)
    std::mutex m_;
    std::condition_variable cv_;
#endif
};

}  // namespace wtc::rt
