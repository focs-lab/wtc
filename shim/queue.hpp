#pragma once

#include <memory>
#include <mutex>
#include <new>

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

    // Exactly one member is alive, the one tag_ names. The union's own
    // constructor and destructor do nothing; the enclosing class constructs
    // and destroys the chosen member explicitly.
    union storage {
        generic_impl generic;
        spsc_impl spsc;
        storage() {}
        ~storage() {}
    };

    topology tag_;
    storage impl_;

public:
    [[clang::annotate("wtc_queue_construction")]]
    explicit queue(topology tag = topology::generic) : tag_(tag) {
        switch (tag_) {
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
        if (tag_ == topology::spsc) {
            impl_.spsc.~spsc_impl();
        } else {
            impl_.generic.~generic_impl();
        }
    }

    topology tag() const { return tag_; }

    [[clang::annotate("wtc_queue_try_pop")]]
    std::shared_ptr<T> try_pop() {
        if (tag_ == topology::spsc) {
            std::shared_ptr<T> out;
            impl_.spsc.try_pop(out);
            return out;
        }
        return impl_.generic.try_pop();
    }

    [[clang::annotate("wtc_queue_push")]]
    void push(const T& new_value) {
        if (tag_ == topology::spsc) {
            impl_.spsc.push(std::make_shared<T>(new_value));
            return;
        }
        impl_.generic.push(new_value);
    }
};

}
