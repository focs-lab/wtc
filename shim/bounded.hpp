#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "topology.hpp"
#include "../runtime/bounded_ring.hpp"
#include "../runtime/waiter.hpp"

namespace wtc {

// A bounded queue: the substitute for a queue the program bounds itself
// (docs/lifting.md, "The bound"). Its capacity is exact, as the original
// ring's under its mutex was (runtime/bounded_ring.hpp), and a push has
// the original's three answers to a full queue:
//
//   push_wait     waits until there is room (spdlog's enqueue)
//   try_push      fails, leaving its argument alone (enqueue_if_have_room,
//                 whose caller then counts a discard)
//   push_overrun  removes the oldest element and appends its own at one
//                 step (enqueue_nowait over circular_q's push_back)
//
// Waiting is on two eventcounts (runtime/waiter.hpp), one per side: a
// consumer waits on not_empty_ while the queue is empty, and every push
// that appended wakes one; a producer waits on not_full_ while it is full,
// and every pop wakes one. Each publish (a ticket taken) is followed by the
// waiter's fence and read, which is what rules out the lost wake-up on
// either side. close() closes both: a waiting pop returns false once the
// queue is closed and empty, a waiting push once it is closed and full;
// neither changes try_push or try_pop.
//
// The topology tag picks the sides that need a compare-and-swap; a queue
// that may overrun takes every ticket with one (the packed layout), since
// its producers are consumers too. The tag is the constructor's first
// argument, which the specialisation pass rewrites as it does
// wtc::queue's.
template <typename T>
class bounded {
public:
    bounded(topology tag, std::size_t capacity, bool overrun, rt::wait_policy policy)
        : tag_(tag),
          ring_(capacity, manyProducers(tag), manyConsumers(tag), overrun),
          not_empty_(policy),
          not_full_(policy) {}

    bounded(const bounded&) = delete;
    bounded& operator=(const bounded&) = delete;

    topology tag() const { return tag_; }
    std::size_t capacity() const { return ring_.capacity(); }

    template <class V>
    bool try_push(V&& v) {
        if (!ring_.try_push(std::forward<V>(v))) return false;
        not_empty_.notify(1);
        return true;
    }

    // Waits while the queue is full; false only once it is closed and still
    // full, with v untouched.
    template <class V>
    bool push_wait(V&& v) {
        if (!not_full_.wait([&]() noexcept { return ring_.try_push(std::forward<V>(v)); })) return false;
        not_empty_.notify(1);
        return true;
    }

    // True when the oldest element was removed to make room.
    template <class V>
    bool push_overrun(V&& v) {
        const bool overran = ring_.push_overrun(std::forward<V>(v));
        not_empty_.notify(1);
        return overran;
    }

    bool try_pop(T& out) {
        if (!take(out)) return false;
        made_room();
        return true;
    }

    bool pop_wait(T& out) {
        if (!not_empty_.wait([&]() noexcept { return take(out); })) return false;
        made_room();
        return true;
    }

    bool pop_wait_for(T& out, std::chrono::nanoseconds limit) {
        if (!not_empty_.wait_for([&]() noexcept { return take(out); }, limit)) return false;
        made_room();
        return true;
    }

    void close() {
        not_empty_.close();
        not_full_.close();
    }
    bool closed() const { return not_empty_.closed(); }

    std::size_t size() const { return ring_.size(); }
    std::size_t overrun_counter() const { return ring_.overrun_counter(); }
    void reset_overrun_counter() { ring_.reset_overrun_counter(); }

    // Hints for tests.
    const rt::waiter& consumers() const { return not_empty_; }
    const rt::waiter& producers() const { return not_full_; }

private:
    static bool manyProducers(topology t) { return t != topology::spsc && t != topology::spsc_inline && t != topology::spmc; }
    static bool manyConsumers(topology t) { return t != topology::spsc && t != topology::spsc_inline && t != topology::mpsc; }

    bool take(T& out) noexcept {
        return ring_.consume([&](T&& e) noexcept { out = std::move(e); });
    }

    // After a pop: one push asleep for room is woken, if one is announced.
    // Not later, once a run of pops has made more room, which would save
    // system calls while the queue stays full: a consumer that pops one
    // element and then waits for what that push's thread sends next would
    // wait for ever, where the original's notify_one let the push through.
    void made_room() noexcept { not_full_.notify(1); }

    const topology tag_;
    rt::bounded_ring<T> ring_;
    // Their own lines: a waiter that goes to sleep or wakes writes them.
    alignas(128) rt::waiter not_empty_;
    alignas(128) rt::waiter not_full_;
};

}  // namespace wtc
