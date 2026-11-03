#pragma once

#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

#include "bounded.hpp"
#include "queue.hpp"

namespace wtc {

// The queue the compiler puts in place of a class the program wrote
// itself: a standard container under a mutex, with condition variables to
// wait on (--wtc-lift-locked-queues, docs/lifting.md). The program never
// names this header. The compiler driver instantiates wtc::lifted<C> for
// the container type C of every class it lifts, and the pass replaces the
// bodies of that class's constructor, destructor, push and pop functions
// with calls to the functions below; nothing else in the program changes.
//
// The class keeps its layout. The container member's storage holds a
// pointer to a wtc::queue<T>, built by construct and freed by destroy; the
// pass proves first that nothing else in the program touches that storage.
// construct takes the topology tag, which the specialisation pass rewrites
// exactly as it rewrites wtc::queue's own constructor's.
//
// The functions annotated wtc_queue_* below are the operations the lifting
// pass raises to the dialect's; the wtc::queue calls inside them are their
// implementation and are not raised again (LiftCIRToWTC.cpp).
//
// A pop that waits keeps the original's sleep: it is wtc::queue's
// pop_wait, built with the spin_then_park policy, which spins briefly and
// then sleeps until an element arrives or the queue is closed; a push
// wakes a sleeper only when one is registered. The class's stop flag, set
// under its mutex together with a broadcast and never cleared, is the
// queue's close(). No wait is ever turned into spinning without end.
template <class C>
struct lifted {
    using T = typename C::value_type;
    using Q = queue<T>;

    static_assert(sizeof(C) >= sizeof(Q*) && alignof(C) >= alignof(Q*),
                  "the container's storage holds the pointer to the queue");

    static Q* get(C* at) { return *std::launder(reinterpret_cast<Q**>(at)); }

    // Read by the pass from its body: whether copying an element out is
    // the same as moving it, which is what wtc::queue's pops do. It returns
    // a literal, which the pass reads without evaluating anything; the
    // argument is unused, and is how the pass tells one C's from another's.
    [[clang::annotate("wtc_lifted_copy_ok")]]
    static bool copy_ok(C*) {
        if constexpr (std::is_trivially_copyable_v<T>) return true;
        else return false;
    }

    // Called by construct exactly when T may be held inline, as
    // wtc::queue's constructor does, so that the specialisation pass reads
    // the same mark.
    [[clang::annotate("wtc_queue_inline_ok")]]
    static void inline_ok() {}

    [[clang::annotate("wtc_queue_construction"), clang::annotate("wtc_lifted_construct")]]
    static void construct(C* at, topology tag) {
        if constexpr (std::is_trivially_destructible_v<T>) inline_ok();
        ::new (static_cast<void*>(at)) Q*(new Q(tag, wait_policy::spin_then_park));
    }

    [[clang::annotate("wtc_lifted_destroy")]]
    static void destroy(C* at) { delete get(at); }

    [[clang::annotate("wtc_queue_push"), clang::annotate("wtc_lifted_push_move")]]
    static void push(C* at, T&& v) { get(at)->push(std::move(v)); }

    // A class that copies its element in. Instantiated with the rest for
    // every C, also for an element that cannot be copied, whose class
    // never copies it in and never calls this.
    [[clang::annotate("wtc_queue_push"), clang::annotate("wtc_lifted_push_copy")]]
    static void push_copy(C* at, const T& v) {
        if constexpr (std::is_copy_constructible_v<T>) get(at)->push(v);
        else std::abort();
    }

    // The element is moved out. Used for a class that copies it out only
    // when copy_ok(), where the two are the same.
    [[clang::annotate("wtc_queue_try_pop_into"), clang::annotate("wtc_lifted_try_pop_move")]]
    static bool try_pop(C* at, T& out) { return get(at)->try_pop(out); }

    // The original's "wait until not empty [or stopped]; take the front":
    // an element, after waiting for as long as there is none; false only
    // once the queue is closed and empty, which a class without a stop
    // flag never makes it, and which is the original's "if empty, return
    // false" after a wait for "not empty or stopped".
    [[clang::annotate("wtc_queue_pop_wait"), clang::annotate("wtc_lifted_pop_wait_move")]]
    static bool pop_wait(C* at, T& out) { return get(at)->pop_wait(out); }

    // The original's "set the stop flag under the mutex, wake every waiter".
    [[clang::annotate("wtc_lifted_close")]]
    static void close(C* at) { get(at)->close(); }
};

// The same for a class whose container is a bounded ring (spdlog's
// circular_q): the container's storage holds a pointer to a wtc::bounded,
// whose capacity is the ring's, built from the argument the class passed
// to the ring's constructor. A push has the three answers to a full queue
// the class wrote (wait, fail, overrun the oldest), each a function below
// with an annotation of its own; the pops are the unbounded ones'.
//
// An overrun removes an element, so a thread that may overrun is a
// consumer as well as a producer: push_overrun is annotated so, and the
// queue is built for it (overrun = true). Where the pass has proved that
// no overrun can run (the overflow policy that would choose it is a value
// it can resolve, and never that one), the class's overrunning push is
// lifted to push_overrun_unreachable instead, which counts as a push only,
// and the queue is built without overruns; reaching it anyway ends the
// program (runtime/bounded_ring.hpp), rather than letting a producer
// consume from a queue proved to have one consumer.
//
// The counters: size() is exact. overrun_counter() is approximate while an
// overrun is in flight (runtime/bounded_ring.hpp), and the pass lifts it
// only where its value is returned to the caller and nothing else.
template <class C>
struct lifted_bounded {
    using T = typename C::value_type;
    using Q = bounded<T>;

    static_assert(sizeof(C) >= sizeof(Q*) && alignof(C) >= alignof(Q*),
                  "the container's storage holds the pointer to the queue");

    static Q* get(C* at) { return *std::launder(reinterpret_cast<Q**>(at)); }

    // The pushes that copy exist for every element type, since the driver
    // instantiates the whole template; for a type that cannot be copied the
    // pass never calls them (the class pushed by moving).
    template <class F>
    static void copy(C* at, const T& v, F&& f) {
        if constexpr (std::is_copy_constructible_v<T>) f(get(at), v);
        else (void)at, (void)v, (void)f, std::abort();
    }

    [[clang::annotate("wtc_lifted_copy_ok")]]
    static bool copy_ok(C*) {
        if constexpr (std::is_trivially_copyable_v<T>) return true;
        else return false;
    }

    [[clang::annotate("wtc_queue_construction"), clang::annotate("wtc_lifted_construct_bounded")]]
    static void construct(C* at, topology tag, std::size_t capacity, bool overrun) {
        ::new (static_cast<void*>(at)) Q*(new Q(tag, capacity, overrun, wait_policy::spin_then_park));
    }

    [[clang::annotate("wtc_lifted_destroy")]]
    static void destroy(C* at) { delete get(at); }

    [[clang::annotate("wtc_queue_push_wait"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_push_wait_move")]]
    static void push_wait(C* at, T&& v) { get(at)->push_wait(std::move(v)); }

    [[clang::annotate("wtc_queue_push_wait"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_push_wait_copy")]]
    static void push_wait_copy(C* at, const T& v) { copy(at, v, [](auto* q, const auto& x) { q->push_wait(x); }); }

    [[clang::annotate("wtc_queue_try_push"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_try_push_move")]]
    static bool try_push(C* at, T&& v) { return get(at)->try_push(std::move(v)); }

    [[clang::annotate("wtc_queue_try_push"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_try_push_copy")]]
    static bool try_push_copy(C* at, const T& v) {
        bool ok = false;
        copy(at, v, [&](auto* q, const auto& x) { ok = q->try_push(x); });
        return ok;
    }

    [[clang::annotate("wtc_queue_push_overrun"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_push_overrun_move")]]
    static void push_overrun(C* at, T&& v) { get(at)->push_overrun(std::move(v)); }

    [[clang::annotate("wtc_queue_push_overrun"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_push_overrun_copy")]]
    static void push_overrun_copy(C* at, const T& v) { copy(at, v, [](auto* q, const auto& x) { q->push_overrun(x); }); }

    [[clang::annotate("wtc_queue_push"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_push_overrun_unreachable_move")]]
    static void push_overrun_unreachable(C* at, T&& v) { get(at)->push_overrun(std::move(v)); }

    [[clang::annotate("wtc_queue_push"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_push_overrun_unreachable_copy")]]
    static void push_overrun_unreachable_copy(C* at, const T& v) {
        copy(at, v, [](auto* q, const auto& x) { q->push_overrun(x); });
    }

    [[clang::annotate("wtc_queue_try_pop_into"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_try_pop_move")]]
    static bool try_pop(C* at, T& out) { return get(at)->try_pop(out); }

    [[clang::annotate("wtc_queue_pop_wait"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_pop_wait_move")]]
    static bool pop_wait(C* at, T& out) { return get(at)->pop_wait(out); }

    // The original's timed wait, by the unit of its duration.
    [[clang::annotate("wtc_queue_pop_wait_for"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_pop_wait_for_ns")]]
    static bool pop_wait_for_ns(C* at, T& out, std::chrono::nanoseconds d) { return get(at)->pop_wait_for(out, d); }

    [[clang::annotate("wtc_queue_pop_wait_for"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_pop_wait_for_us")]]
    static bool pop_wait_for_us(C* at, T& out, std::chrono::microseconds d) { return get(at)->pop_wait_for(out, d); }

    [[clang::annotate("wtc_queue_pop_wait_for"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_pop_wait_for_ms")]]
    static bool pop_wait_for_ms(C* at, T& out, std::chrono::milliseconds d) { return get(at)->pop_wait_for(out, d); }

    [[clang::annotate("wtc_queue_pop_wait_for"), clang::annotate("wtc_queue_bounded"),
      clang::annotate("wtc_lifted_pop_wait_for_s")]]
    static bool pop_wait_for_s(C* at, T& out, std::chrono::seconds d) { return get(at)->pop_wait_for(out, d); }

    [[clang::annotate("wtc_lifted_close")]]
    static void close(C* at) { get(at)->close(); }

    [[clang::annotate("wtc_lifted_size")]]
    static std::size_t size(C* at) { return get(at)->size(); }

    [[clang::annotate("wtc_lifted_overrun_counter")]]
    static std::size_t overrun_counter(C* at) { return get(at)->overrun_counter(); }

    [[clang::annotate("wtc_lifted_reset_overrun_counter")]]
    static void reset_overrun_counter(C* at) { get(at)->reset_overrun_counter(); }
};

}  // namespace wtc
