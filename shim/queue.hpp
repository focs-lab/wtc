#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "topology.hpp"
#include "../runtime/segment_queue.hpp"
#include "../runtime/spmc_ring.hpp"
#include "../runtime/spsc_ring.hpp"

#ifndef WTC_BATCH_K
#define WTC_BATCH_K 64
#endif

namespace wtc {

// A queue with one API and several implementations, chosen per object at
// construction by a topology tag. The general-purpose one is a list of array
// segments whose cells producers and consumers take with one fetch-and-add
// each (runtime/segment_queue.hpp), and the one for many producers and one
// consumer (mpsc) the same queue with its consumer's side unshared; the
// single-producer single-consumer one is a wait-free ring from runtime/,
// and the one for one producer and many consumers the same ring with its
// consumers taking turns (spmc). The tag is the whole substitution
// mechanism: the compiler rewrites the constant at one constructor call and
// every operation on that object follows it through one predictable branch.
//
// Every implementation holds the elements themselves in its cells: a push
// copies or moves its argument into the cell, and nothing is allocated per
// element on the producer's side. What a pop does with the element is the
// element's ownership, and there are three kinds:
//
//   - Transfer: try_pop(T&) moves the element out of the cell into the
//     consumer's object. No allocation on either side.
//   - Share: try_pop() returns a shared_ptr<T>, the original API, and
//     allocates the element it returns, on the consumer's side, from the
//     cell. The memory that crosses the threads is the cell's, which the
//     queue owns and reuses; the allocation and the free are on the same
//     thread. That is the form every program written against the original
//     queue uses, and it is what the compiler sees unless it proves more.
//   - Borrow: with the spsc_inline tag, try_pop() hands the element out in
//     place through a shared_ptr that owns nothing (the aliasing constructor
//     over an empty owner). That pointer is valid until the consumer's next
//     try_pop, which is the contract the compiler proves before choosing
//     the tag; the element is destroyed in place then, so only a trivially
//     destructible type may be lent. With WTC_CHECK the pointer shares a
//     per-queue guard instead, and a try_pop that finds it still referenced
//     aborts.
//
// A run of pushes from one producer can be batched: batch_begin, then
// push_deferred for each element, then batch_end. The elements become
// visible to consumers together, in order, at batch_end or every
// WTC_BATCH_K elements, whichever comes first; until then they are the
// producer's. One fetch-and-add (general-purpose and mpsc) or one release
// store (the rings) per run instead of one per element.
// The compiler rewrites a loop of pushes into this shape where the program
// allows it (WTC_BATCH, shim/progress.hpp); a program may also write it by
// hand.
template <typename T>
class queue {
private:
    // The original queue's two-lock list is benchmarks/queues/
    // generic_original.hpp, kept as a baseline.
    using generic_impl = rt::segment_queue<T, true>;
    using mpsc_impl = rt::segment_queue<T, false>;
    using ring_impl = rt::spsc_ring<T>;  // spsc and spsc_inline
    using spmc_impl = rt::spmc_ring<T>;

    // Lending needs the element's end to be invisible: it is destroyed in
    // place at the next pop, not when the last shared_ptr to it goes.
    static constexpr bool kInlineOk = std::is_trivially_destructible_v<T>;

    // Called by the constructor only when kInlineOk holds, so that the
    // compiler can tell from the constructor's body whether this element
    // type may be lent. It is a call rather than an argument of the
    // constructor's annotation because ClangIR does not carry annotation
    // arguments yet.
    [[clang::annotate("wtc_queue_inline_ok")]]
    static void inline_ok() {}

    // Exactly one member is alive, the one tag_ names. The union's own
    // constructor and destructor do nothing; the enclosing class constructs
    // and destroys the chosen member explicitly.
    union storage {
        generic_impl generic;
        ring_impl ring;
        mpsc_impl mpsc;
        spmc_impl spmc;
        storage() {}
        ~storage() {}
    };

    // The tag is read by every thread on every operation and never
    // written after construction; the implementation's first line is the
    // producer's private state. The two must not share a 128-byte pair, or
    // the consumer's reads of the tag keep pulling the producer's line into
    // its cache through the adjacent-line prefetcher and every push pays
    // to take it back.
    alignas(128) topology tag_;
    alignas(128) storage impl_;
#ifdef WTC_CHECK
    // Shares ownership with every element lent by an spsc_inline queue;
    // anything above one reference is a consumer still holding one.
    std::shared_ptr<void> guard_;

    void check_returned(const char* when) const {
        if (tag_ == topology::spsc_inline && guard_.use_count() != 1) {
            std::fprintf(stderr,
                         "wtc: borrowed element still referenced %s on queue %p\n",
                         when, static_cast<const void*>(this));
            std::abort();
        }
    }
#endif

public:
    // A producer's run of deferred pushes. Raw storage that batch_begin
    // brings to life and batch_end ends, so that it can live in memory the
    // compiler set aside without running a constructor, and costs nothing
    // to declare by hand.
    class batch {
        friend class queue;
        struct state {
            // General-purpose and mpsc: the run so far, constructed in
            // place as it grows and pushed with one fetch-and-add.
            alignas(T) unsigned char run_raw[WTC_BATCH_K * sizeof(T)];
            T* run_at(unsigned i) { return std::launder(reinterpret_cast<T*>(run_raw)) + i; }
            unsigned pending = 0;
        };
        alignas(state) unsigned char raw[sizeof(state)];
        state& get() { return *std::launder(reinterpret_cast<state*>(raw)); }
    };

    [[clang::annotate("wtc_queue_construction")]]
    explicit queue(topology tag = topology::generic) : tag_(tag) {
        if constexpr (kInlineOk) inline_ok();
        switch (tag_) {
        case topology::spsc_inline:
            if constexpr (kInlineOk) {
#ifdef WTC_CHECK
                guard_ = std::shared_ptr<void>(static_cast<void*>(this), [](void*) {});
#endif
            } else {
                tag_ = topology::spsc;
            }
            new (&impl_.ring) ring_impl();
            break;
        case topology::spsc:
            new (&impl_.ring) ring_impl();
            break;
        case topology::mpsc:
            new (&impl_.mpsc) mpsc_impl();
            break;
        case topology::spmc:
            new (&impl_.spmc) spmc_impl();
            break;
        case topology::generic:
        default:
            tag_ = topology::generic;
            new (&impl_.generic) generic_impl();
            break;
        }
    }

    queue(const queue&) = delete;
    queue& operator=(const queue&) = delete;

    queue(queue&&) = delete;
    queue& operator=(queue&&) = delete;

    ~queue() {
        switch (tag_) {
        case topology::spsc_inline:
#ifdef WTC_CHECK
            check_returned("when the queue is destroyed");
#endif
            impl_.ring.~ring_impl();
            break;
        case topology::spsc:
            impl_.ring.~ring_impl();
            break;
        case topology::mpsc:
            impl_.mpsc.~mpsc_impl();
            break;
        case topology::spmc:
            impl_.spmc.~spmc_impl();
            break;
        default:
            impl_.generic.~generic_impl();
            break;
        }
    }

    topology tag() const { return tag_; }

private:
    // Makes the run so far visible: one release store on a ring, one
    // fetch-and-add for the run on the segmented queues; in each, the order
    // the same pushes made one by one would give.
    void publish(typename batch::state& st) {
        const unsigned n = st.pending;
        st.pending = 0;
        switch (tag_) {
        case topology::spsc_inline:
        case topology::spsc:
            impl_.ring.publish();
            return;
        case topology::mpsc:
            if (!n) return;
            impl_.mpsc.push_run(st.run_at(0), n);
            for (unsigned i = 0; i < n; ++i) st.run_at(i)->~T();
            return;
        case topology::spmc:
            impl_.spmc.publish();
            return;
        default:
            if (!n) return;
            impl_.generic.push_run(st.run_at(0), n);
            for (unsigned i = 0; i < n; ++i) st.run_at(i)->~T();
            return;
        }
    }

    // The oldest element to f, whichever implementation holds it.
    template <class F>
    bool consume(F&& f) {
        switch (tag_) {
        case topology::spsc_inline:
        case topology::spsc:
            return impl_.ring.consume(f);
        case topology::mpsc:
            return impl_.mpsc.consume(f);
        case topology::spmc:
            return impl_.spmc.consume(f);
        default:
            return impl_.generic.consume(f);
        }
    }

public:

    // Share, or borrow under the spsc_inline tag: the oldest element as a
    // shared_ptr, null when the queue was empty.
    [[clang::annotate("wtc_queue_try_pop")]]
    std::shared_ptr<T> try_pop() {
        if (tag_ == topology::spsc_inline) {
#ifdef WTC_CHECK
            check_returned("at the next try_pop");
#endif
            T* p = impl_.ring.try_pop_borrow();
            if (!p) return std::shared_ptr<T>();
#ifdef WTC_CHECK
            return std::shared_ptr<T>(guard_, p);
#else
            return std::shared_ptr<T>(std::shared_ptr<void>(), p);
#endif
        }
        // The queues call the taker with the element still in its cell and
        // release the cell afterwards, so the taker must not throw: a
        // failed allocation here ends the program, as it would have in the
        // original queue's push.
        std::shared_ptr<T> out;
        consume([&](T&& e) noexcept { out = std::make_shared<T>(std::move(e)); });
        return out;
    }

    // Transfer: the oldest element moved into out; false when the queue was
    // empty, and out is then untouched.
    [[clang::annotate("wtc_queue_try_pop_into")]]
    bool try_pop(T& out) {
#ifdef WTC_CHECK
        check_returned("at the next try_pop");
#endif
        return consume([&](T&& e) noexcept { out = std::move(e); });
    }

    // gnu::used: the compiler calls these where the program allows it, so
    // they must be in every module that uses the queue, called or not.
    [[gnu::used, clang::annotate("wtc_queue_batch_begin")]]
    void batch_begin(batch& b) { ::new (static_cast<void*>(b.raw)) typename batch::state(); }

    [[gnu::used, clang::annotate("wtc_queue_push_deferred")]]
    void push_deferred(batch& b, const T& new_value) {
        // A type that cannot be copied is only ever pushed by moving, which
        // the overload below batches; this body exists for the symbol.
        if constexpr (std::is_copy_constructible_v<T>) {
            typename batch::state& st = b.get();
            switch (tag_) {
            case topology::spsc_inline:
            case topology::spsc:
                impl_.ring.push_deferred(new_value);
                break;
            case topology::spmc:
                impl_.spmc.push_deferred(new_value);
                break;
            case topology::mpsc:
            default:
                ::new (static_cast<void*>(st.run_at(st.pending))) T(new_value);
                break;
            }
            if (++st.pending == WTC_BATCH_K) publish(st);
        } else {
            (void)b; (void)new_value;
            std::abort();
        }
    }

    // The same for a push by rvalue: the element is moved in. The batching
    // pass picks this overload for a push by rvalue and the copying one for
    // a push by const reference, by the overloads' mangled names.
    [[gnu::used, clang::annotate("wtc_queue_push_deferred")]]
    void push_deferred(batch& b, T&& new_value) {
        typename batch::state& st = b.get();
        switch (tag_) {
        case topology::spsc_inline:
        case topology::spsc:
            impl_.ring.push_deferred(std::move(new_value));
            break;
        case topology::spmc:
            impl_.spmc.push_deferred(std::move(new_value));
            break;
        case topology::mpsc:
        default:
            ::new (static_cast<void*>(st.run_at(st.pending))) T(std::move(new_value));
            break;
        }
        if (++st.pending == WTC_BATCH_K) publish(st);
    }

    [[gnu::used, clang::annotate("wtc_queue_batch_end")]]
    void batch_end(batch& b) {
        publish(b.get());
        b.get().~state();
    }

    [[clang::annotate("wtc_queue_push")]]
    void push(const T& new_value) {
        switch (tag_) {
        case topology::spsc_inline:
        case topology::spsc:
            impl_.ring.push(new_value);
            return;
        case topology::mpsc:
            impl_.mpsc.push(new_value);
            return;
        case topology::spmc:
            impl_.spmc.push(new_value);
            return;
        default:
            impl_.generic.push(new_value);
            return;
        }
    }

    [[clang::annotate("wtc_queue_push")]]
    void push(T&& new_value) {
        switch (tag_) {
        case topology::spsc_inline:
        case topology::spsc:
            impl_.ring.push(std::move(new_value));
            return;
        case topology::mpsc:
            impl_.mpsc.push(std::move(new_value));
            return;
        case topology::spmc:
            impl_.spmc.push(std::move(new_value));
            return;
        default:
            impl_.generic.push(std::move(new_value));
            return;
        }
    }
};

}
