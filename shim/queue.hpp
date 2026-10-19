#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>

#include "topology.hpp"
#include "../runtime/spsc_ring.hpp"

namespace wtc {

// A queue with one API and two implementations, chosen per object at
// construction by a topology tag. The general-purpose one is a two-lock
// linked list; the single-producer single-consumer one is a wait-free ring
// from runtime/. The tag is the whole substitution mechanism: the compiler
// rewrites the constant at one constructor call and every operation on that
// object follows it through one predictable branch.
//
// The API is that of the original queue: push copies its argument into a
// shared_ptr, try_pop hands that shared_ptr back or a null one when empty.
//
// A third implementation, spsc_inline, holds the elements inline in the
// ring and hands each one out in place through a shared_ptr that owns
// nothing (the aliasing constructor over an empty owner): no allocation on
// either side. That pointer is valid until the consumer's next try_pop,
// which is the contract the compiler proves before choosing it. With
// WTC_CHECK the pointer shares a per-queue guard instead, and a try_pop
// that finds it still referenced aborts.
template <typename T>
class queue {
private:
    // The general-purpose implementation, verbatim from the original queue
    // (see benchmarks/queues/generic_original.hpp for that file as it was).
    struct generic_impl {
        struct node {
            std::shared_ptr<T> data;
            std::unique_ptr<node> next;
        };

        std::unique_ptr<node> head;
        node* tail;
        std::mutex head_mutex;
        std::mutex tail_mutex;

        generic_impl() : head(new node), tail(head.get()) {}

        node* get_tail() {
            std::lock_guard<std::mutex> tail_lock(tail_mutex);
            return tail;
        }

        std::unique_ptr<node> pop_head() {
            std::lock_guard<std::mutex> head_lock(head_mutex);
            if (head.get() == get_tail()) {
                return nullptr;
            }
            std::unique_ptr<node> old_head = std::move(head);
            head = std::move(old_head->next);
            return old_head;
        }

        std::shared_ptr<T> try_pop() {
            std::unique_ptr<node> old_head = pop_head();
            return old_head ? old_head->data : std::shared_ptr<T>();
        }

        void push(const T& new_value) {
            std::shared_ptr<T> new_data(std::make_shared<T>(std::move(new_value)));
            std::unique_ptr<node> p(new node);
            node* const new_tail = p.get();
            std::lock_guard<std::mutex> tail_lock(tail_mutex);
            tail->data = new_data;
            tail->next = std::move(p);
            tail = new_tail;
        }
    };

    using spsc_impl = rt::spsc_ring<std::shared_ptr<T>>;
    using inline_impl = rt::spsc_ring<T>;

    // Inline storage needs the element's end to be invisible: it is
    // destroyed in place at the next pop, not when the last shared_ptr to
    // it goes, so only a trivially destructible type may be held inline.
    static constexpr bool kInlineOk =
        std::is_trivially_destructible_v<T> && std::is_copy_constructible_v<T>;

    // Exactly one member is alive, the one tag_ names. The union's own
    // constructor and destructor do nothing; the enclosing class constructs
    // and destroys the chosen member explicitly.
    union storage {
        generic_impl generic;
        spsc_impl spsc;
        inline_impl inl;
        storage() {}
        ~storage() {}
    };

    topology tag_;
    storage impl_;
#ifdef WTC_CHECK
    // Shares ownership with every element handed out by an spsc_inline
    // queue; anything above one reference is a consumer still holding one.
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
    [[clang::annotate("wtc_queue_construction")]]
    explicit queue(topology tag = topology::generic) : tag_(tag) {
        switch (tag_) {
        case topology::spsc_inline:
            if constexpr (kInlineOk) {
                new (&impl_.inl) inline_impl();
#ifdef WTC_CHECK
                guard_ = std::shared_ptr<void>(static_cast<void*>(this), [](void*) {});
#endif
                break;
            }
            tag_ = topology::spsc;
            new (&impl_.spsc) spsc_impl();
            break;
        case topology::spsc:
            new (&impl_.spsc) spsc_impl();
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
            impl_.inl.~inline_impl();
            break;
        case topology::spsc:
            impl_.spsc.~spsc_impl();
            break;
        default:
            impl_.generic.~generic_impl();
            break;
        }
    }

    topology tag() const { return tag_; }

    [[clang::annotate("wtc_queue_try_pop")]]
    std::shared_ptr<T> try_pop() {
        if (tag_ == topology::spsc_inline) {
#ifdef WTC_CHECK
            check_returned("at the next try_pop");
#endif
            T* p = impl_.inl.try_pop_borrow();
            if (!p) return std::shared_ptr<T>();
#ifdef WTC_CHECK
            return std::shared_ptr<T>(guard_, p);
#else
            return std::shared_ptr<T>(std::shared_ptr<void>(), p);
#endif
        }
        if (tag_ == topology::spsc) {
            std::shared_ptr<T> out;
            impl_.spsc.try_pop(out);
            return out;
        }
        return impl_.generic.try_pop();
    }

    [[clang::annotate("wtc_queue_push")]]
    void push(const T& new_value) {
        if (tag_ == topology::spsc_inline) {
            if constexpr (kInlineOk) impl_.inl.push(new_value);
            return;
        }
        if (tag_ == topology::spsc) {
            impl_.spsc.push(std::make_shared<T>(new_value));
            return;
        }
        impl_.generic.push(new_value);
    }
};

}
