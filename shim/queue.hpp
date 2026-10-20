#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>

#include "topology.hpp"
#include "../runtime/mpsc_list.hpp"
#include "../runtime/spsc_ring.hpp"

#ifndef WTC_BATCH_K
#define WTC_BATCH_K 64
#endif

namespace wtc {

// A queue with one API and several implementations, chosen per object at
// construction by a topology tag. The general-purpose one is a two-lock
// linked list; the single-producer single-consumer one is a wait-free ring
// from runtime/, and the one for many producers and one consumer a list
// without locks (mpsc). The tag is the whole substitution mechanism: the
// compiler rewrites the constant at one constructor call and every
// operation on that object follows it through one predictable branch.
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
//
// A run of pushes from one producer can be batched: batch_begin, then
// push_deferred for each element, then batch_end. The elements become
// visible to consumers together, in order, at batch_end or every
// WTC_BATCH_K elements, whichever comes first; until then they are the
// producer's. One mutex acquisition (general-purpose), one release store
// (the rings) or one exchange (mpsc) per run instead of one per element.
// The compiler rewrites a loop of pushes into this shape where the program
// allows it (WTC_BATCH, shim/progress.hpp); a program may also write it by
// hand.
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
    using mpsc_impl = rt::mpsc_list<std::shared_ptr<T>>;

    // Inline storage needs the element's end to be invisible: it is
    // destroyed in place at the next pop, not when the last shared_ptr to
    // it goes, so only a trivially destructible type may be held inline.
    static constexpr bool kInlineOk =
        std::is_trivially_destructible_v<T> && std::is_copy_constructible_v<T>;

    // Called by the constructor only when kInlineOk holds, so that the
    // compiler can tell from the constructor's body whether this element
    // type may be held inline. It is a call rather than an argument of the
    // constructor's annotation because ClangIR does not carry annotation
    // arguments yet.
    [[clang::annotate("wtc_queue_inline_ok")]]
    static void inline_ok() {}

    // Exactly one member is alive, the one tag_ names. The union's own
    // constructor and destructor do nothing; the enclosing class constructs
    // and destroys the chosen member explicitly.
    union storage {
        generic_impl generic;
        spsc_impl spsc;
        inline_impl inl;
        mpsc_impl mpsc;
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
    // A producer's run of deferred pushes. Raw storage that batch_begin
    // brings to life and batch_end ends, so that it can live in memory the
    // compiler set aside without running a constructor, and costs nothing
    // to declare by hand.
    class batch {
        friend class queue;
        struct state {
            // General-purpose: the element for the queue's current dummy
            // node, and the chain that follows it, ending in a new dummy.
            std::shared_ptr<T> first;
            std::unique_ptr<typename generic_impl::node> rest;
            typename generic_impl::node* rest_tail = nullptr;
            // mpsc: the run linked so far.
            typename mpsc_impl::chain run;
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
        case topology::mpsc:
            new (&impl_.mpsc) mpsc_impl();
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
        case topology::mpsc:
            impl_.mpsc.~mpsc_impl();
            break;
        default:
            impl_.generic.~generic_impl();
            break;
        }
    }

    topology tag() const { return tag_; }

private:
    // Makes the run so far visible: one release store on a ring, one splice
    // under the tail lock on the general-purpose list, whose result is the
    // list the same pushes made one by one would have built.
    void publish(typename batch::state& st) {
        st.pending = 0;
        switch (tag_) {
        case topology::spsc_inline:
            if constexpr (kInlineOk) impl_.inl.publish();
            return;
        case topology::spsc:
            impl_.spsc.publish();
            return;
        case topology::mpsc:
            impl_.mpsc.publish(st.run);
            return;
        default:
            if (!st.first) return;
            {
                std::lock_guard<std::mutex> tail_lock(impl_.generic.tail_mutex);
                impl_.generic.tail->data = std::move(st.first);
                impl_.generic.tail->next = std::move(st.rest);
                impl_.generic.tail = st.rest_tail;
            }
            st.rest_tail = nullptr;
            return;
        }
    }

public:

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
        if (tag_ == topology::mpsc) {
            std::shared_ptr<T> out;
            impl_.mpsc.try_pop(out);
            return out;
        }
        return impl_.generic.try_pop();
    }

    // gnu::used: the compiler calls these where the program allows it, so
    // they must be in every module that uses the queue, called or not.
    [[gnu::used, clang::annotate("wtc_queue_batch_begin")]]
    void batch_begin(batch& b) { ::new (static_cast<void*>(b.raw)) typename batch::state(); }

    [[gnu::used, clang::annotate("wtc_queue_push_deferred")]]
    void push_deferred(batch& b, const T& new_value) {
        typename batch::state& st = b.get();
        switch (tag_) {
        case topology::spsc_inline:
            if constexpr (kInlineOk) impl_.inl.push_deferred(new_value);
            break;
        case topology::spsc:
            impl_.spsc.push_deferred(std::make_shared<T>(new_value));
            break;
        case topology::mpsc:
            impl_.mpsc.push_deferred(st.run, std::make_shared<T>(new_value));
            break;
        default: {
            // The nodes and the element are allocated here, outside the
            // lock; only the splice at the end of the run takes it.
            using node = typename generic_impl::node;
            if (!st.first) {
                st.first = std::make_shared<T>(new_value);
                st.rest.reset(new node);
                st.rest_tail = st.rest.get();
            } else {
                st.rest_tail->data = std::make_shared<T>(new_value);
                st.rest_tail->next.reset(new node);
                st.rest_tail = st.rest_tail->next.get();
            }
            break;
        }
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
        if (tag_ == topology::spsc_inline) {
            if constexpr (kInlineOk) impl_.inl.push(new_value);
            return;
        }
        if (tag_ == topology::spsc) {
            impl_.spsc.push(std::make_shared<T>(new_value));
            return;
        }
        if (tag_ == topology::mpsc) {
            impl_.mpsc.push(std::make_shared<T>(new_value));
            return;
        }
        impl_.generic.push(new_value);
    }
};

}
