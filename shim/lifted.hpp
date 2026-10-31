#pragma once

#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

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

}  // namespace wtc
