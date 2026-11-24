#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "topology.hpp"
#include "../runtime/bounded_ring.hpp"
#include "../runtime/index_ring.hpp"
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
//
// Two tags choose another queue, for a path that requires a push or a pop
// not to wait for another thread: scq, the lock-free ring of indices, and
// wcq, the wait-free one (runtime/index_ring.hpp), both its exact
// variant, built for kIndexThreads threads. Their capacity counts a push
// in flight, and a pop that has taken its element and not yet given its
// place back, as holding a place for the pushes, not for the pops (the
// header states the histories this admits), and they do not overrun: a
// queue built to overrun keeps the ring whatever its tag. The constructor
// announces them to the compiler (offers_scq, offers_wcq) with the tag and
// the bound the substitution needs: at most kIndexThreads operations in
// flight at once on the queue, one that an element's constructor, move or
// destructor starts on the same queue counting apart, and on a wcq queue
// also at most kIndexThreads threads alive at once that have operated on
// it. A compiler that cannot show its count within the bound must not
// choose the tag. Past it, the exact variant over either ring may abort
// with no free cell, and the wcq queue's extra threads run lock-free
// fallbacks, which take the wait-freedom of every thread's operations
// while they last; built with WTC_CHECK, both abort as soon as
// the bound is passed. wcq is announced only where the build's
// double-width compare-and-swap is one instruction, and without it the
// tag runs as scq.
//
// Three more choose the settled ring, for a path that requires one side
// not to wait for the other: spsc_settled, mpsc_settled and spmc_settled,
// the ring with those sides whose single side publishes its counter only
// once its cell is filled or emptied (runtime/bounded_ring.hpp, built
// settled). With one producer no pop waits for a push, with one consumer
// no push waits for a pop; the many side's wait remains. They are
// announced as well (offers_*_settled); a queue built to overrun keeps the
// ring whatever its tag.
template <typename T>
class bounded {
public:
    static constexpr unsigned kIndexThreads = 128;

    bounded(topology tag, std::size_t capacity, bool overrun, rt::wait_policy policy)
        : tag_(tag), kind_(kindOf(tag, overrun)), not_empty_(policy), not_full_(policy) {
        offers_scq(static_cast<int>(topology::scq), kIndexThreads);
        if constexpr (rt::kCas2WaitFree) offers_wcq(static_cast<int>(topology::wcq), kIndexThreads);
        offers_spsc_settled(static_cast<int>(topology::spsc_settled));
        offers_mpsc_settled(static_cast<int>(topology::mpsc_settled));
        offers_spmc_settled(static_cast<int>(topology::spmc_settled));
        switch (kind_) {
        case kind::scq: ::new (&impl_.scq) scq_q(capacity, true, kIndexThreads); break;
        case kind::wcq:
            if constexpr (rt::kCas2WaitFree) ::new (&impl_.wcq) wcq_q(capacity, true, kIndexThreads);
            break;
        default:
            ::new (&impl_.ring) ring_q(capacity, manyProducers(tag), manyConsumers(tag), overrun, false, settled(tag));
            break;
        }
    }

    ~bounded() {
        switch (kind_) {
        case kind::scq: impl_.scq.~scq_q(); break;
        case kind::wcq:
            if constexpr (rt::kCas2WaitFree) impl_.wcq.~wcq_q();
            break;
        default: impl_.ring.~ring_q(); break;
        }
    }

    bounded(const bounded&) = delete;
    bounded& operator=(const bounded&) = delete;

    topology tag() const { return tag_; }
    std::size_t capacity() const {
        return with([](auto& q) { return q.capacity(); });
    }

    template <class V>
    bool try_push(V&& v) {
        if (!push_once(std::forward<V>(v))) return false;
        not_empty_.notify(1);
        return true;
    }

    // Waits while the queue is full; false only once it is closed and still
    // full, with v untouched.
    template <class V>
    bool push_wait(V&& v) {
        if (!not_full_.wait([&]() noexcept { return push_once(std::forward<V>(v)); })) return false;
        not_empty_.notify(1);
        return true;
    }

    // True when the oldest element was removed to make room. Only on a
    // queue built to overrun, which is always the ring.
    template <class V>
    bool push_overrun(V&& v) {
        const bool overran = impl_.ring.push_overrun(std::forward<V>(v));
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

    std::size_t size() const {
        return with([](auto& q) { return q.size(); });
    }
    std::size_t overrun_counter() const { return kind_ == kind::ring ? impl_.ring.overrun_counter() : 0; }
    void reset_overrun_counter() {
        if (kind_ == kind::ring) impl_.ring.reset_overrun_counter();
    }

    // Hints for tests.
    const rt::waiter& consumers() const { return not_empty_; }
    const rt::waiter& producers() const { return not_full_; }

private:
    using ring_q = rt::bounded_ring<T>;
    using scq_q = rt::index_queue<T>;
    using wcq_q = rt::index_queue<T, rt::no_ring_hooks, rt::wcq_default>;
    enum class kind : unsigned char { ring, scq, wcq };

    // The markers: the tag, then the bound on threads (see the top).
    [[clang::annotate("wtc_queue_impl_scq")]]
    static void offers_scq(int, unsigned) {}
    [[clang::annotate("wtc_queue_impl_wcq")]]
    static void offers_wcq(int, unsigned) {}
    // The tag alone.
    [[clang::annotate("wtc_queue_impl_spsc_settled")]]
    static void offers_spsc_settled(int) {}
    [[clang::annotate("wtc_queue_impl_mpsc_settled")]]
    static void offers_mpsc_settled(int) {}
    [[clang::annotate("wtc_queue_impl_spmc_settled")]]
    static void offers_spmc_settled(int) {}

    static kind kindOf(topology t, bool overrun) {
        if (overrun) return kind::ring;
        if (t == topology::wcq) return rt::kCas2WaitFree ? kind::wcq : kind::scq;
        if (t == topology::scq) return kind::scq;
        return kind::ring;
    }

    // Calls f with the queue this one carries.
    template <class F>
    decltype(auto) with(F&& f) const {
        auto& self = const_cast<bounded&>(*this);
        if (kind_ == kind::scq) return f(self.impl_.scq);
        if constexpr (rt::kCas2WaitFree)
            if (kind_ == kind::wcq) return f(self.impl_.wcq);
        return f(self.impl_.ring);
    }

    template <class V>
    bool push_once(V&& v) noexcept {
        return with([&](auto& q) noexcept { return q.try_push(std::forward<V>(v)); });
    }

    static bool settled(topology t) {
        return t == topology::spsc_settled || t == topology::mpsc_settled || t == topology::spmc_settled;
    }
    static bool manyProducers(topology t) {
        return t != topology::spsc && t != topology::spsc_inline && t != topology::spmc &&
               t != topology::spsc_settled && t != topology::spmc_settled;
    }
    static bool manyConsumers(topology t) {
        return t != topology::spsc && t != topology::spsc_inline && t != topology::mpsc &&
               t != topology::spsc_settled && t != topology::mpsc_settled;
    }

    bool take(T& out) noexcept {
        return with([&](auto& q) noexcept { return q.consume([&](T&& e) noexcept { out = std::move(e); }); });
    }

    // After a pop: one push asleep for room is woken, if one is announced.
    // Not later, once a run of pops has made more room, which would save
    // system calls while the queue stays full: a consumer that pops one
    // element and then waits for what that push's thread sends next would
    // wait for ever, where the original's notify_one let the push through.
    void made_room() noexcept { not_full_.notify(1); }

    const topology tag_;
    const kind kind_;
    // Exactly one member is alive, the one kind_ names.
    union storage {
        storage() {}
        ~storage() {}
        ring_q ring;
        scq_q scq;
        wcq_q wcq;
    } impl_;
    // Their own lines: a waiter that goes to sleep or wakes writes them.
    alignas(128) rt::waiter not_empty_;
    alignas(128) rt::waiter not_full_;
};

}  // namespace wtc
