#pragma once

// An unbounded single-producer, single-consumer queue, substituted by
// --select-queue-impl for a wtc::queue<T> proven SPSC.
//
// The fact the compiler must prove: over the whole execution, at most one
// thread ever calls push and at most one thread ever calls try_pop. They may
// be the same thread.
//
// What breaks if that is false: two concurrent pushers race on the producer
// cursor and lose or duplicate items, or corrupt a block; two concurrent
// poppers can return one item twice and free a block twice. This is a data
// race, not a slowdown.
//
// It is unbounded, like wtc::queue<T>, so that the substitution is exact:
// push never fails and never blocks. A bounded ring would have to drop an
// item, or wait for the consumer, when full; waiting deadlocks a program in
// which one thread is both the producer and the consumer, which the SPSC
// verdict allows.
//
// Storage is a linked list of fixed-size blocks. Each block counts how many
// of its slots the producer has published. The consumer frees a block only
// once it has drained it and seen the next one; publishing `next` is the
// producer's last access to a block, so nothing touches a freed block.
//
// Measured numbers: not yet. See benchmarks/README.md once they are taken at
// the pinned toolchain.

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace wtc::runtime {

template <typename T, std::size_t BlockSize = 1024>
class spsc_queue {
    static_assert(BlockSize >= 1, "spsc_queue needs at least one slot per block");

    struct slot {
        alignas(T) unsigned char bytes[sizeof(T)];

        T *get() { return std::launder(reinterpret_cast<T *>(bytes)); }
    };

    struct block {
        // Slots [0, written) hold constructed items. Written only by the
        // producer, with release, so an acquire read sees those items.
        std::atomic<std::size_t> written{0};
        std::atomic<block *> next{nullptr};
        slot slots[BlockSize];
    };

    // The consumer's cursor and the producer's cursor are on separate cache
    // lines, so neither side's updates invalidate the other's.
    struct alignas(64) consumer_side {
        block *head;
        std::size_t index = 0;
    };
    struct alignas(64) producer_side {
        block *tail;
        std::size_t index = 0;
    };

    consumer_side consumer_;
    producer_side producer_;

public:
    [[clang::annotate("wtc_impl_spsc_construction")]]
    spsc_queue() {
        block *first = new block;
        consumer_.head = first;
        producer_.tail = first;
    }

    spsc_queue(const spsc_queue &) = delete;
    spsc_queue &operator=(const spsc_queue &) = delete;

    spsc_queue(spsc_queue &&) = delete;
    spsc_queue &operator=(spsc_queue &&) = delete;

    // No other thread may be using the queue by now, so plain loads are
    // enough: the joins or whatever ended the threads ordered their writes.
    [[clang::annotate("wtc_impl_spsc_dtor")]]
    ~spsc_queue() {
        block *b = consumer_.head;
        std::size_t from = consumer_.index;
        while (b) {
            std::size_t written = b->written.load(std::memory_order_relaxed);
            for (std::size_t i = from; i < written; ++i)
                b->slots[i].get()->~T();
            block *next = b->next.load(std::memory_order_relaxed);
            delete b;
            b = next;
            from = 0;
        }
    }

    [[clang::annotate("wtc_impl_spsc_push")]]
    void push(const T &new_value) {
        if (producer_.index == BlockSize) {
            block *fresh = new block;
            producer_.tail->next.store(fresh, std::memory_order_release);
            producer_.tail = fresh;
            producer_.index = 0;
        }

        std::size_t i = producer_.index;
        ::new (static_cast<void *>(producer_.tail->slots[i].bytes)) T(new_value);
        producer_.tail->written.store(i + 1, std::memory_order_release);
        producer_.index = i + 1;
    }

    // The allocation-free form, for code that names spsc_queue directly.
    std::optional<T> try_pop_optional() {
        while (true) {
            block *b = consumer_.head;
            std::size_t i = consumer_.index;

            if (i < BlockSize) {
                if (i == b->written.load(std::memory_order_acquire))
                    return std::nullopt;

                T *item = b->slots[i].get();
                std::optional<T> result(std::move(*item));
                item->~T();
                consumer_.index = i + 1;
                return result;
            }

            // This block is drained. The producer moves on only after
            // filling it, so if there is no next block, the queue is empty.
            block *next = b->next.load(std::memory_order_acquire);
            if (!next)
                return std::nullopt;
            consumer_.head = next;
            consumer_.index = 0;
            delete b;
        }
    }

    // The form substituted for wtc::queue<T>::try_pop, whose callers expect a
    // shared_ptr, null when the queue is empty. Matching that interface costs
    // one allocation per item; try_pop_optional above avoids it.
    [[clang::annotate("wtc_impl_spsc_try_pop")]]
    std::shared_ptr<T> try_pop() {
        std::optional<T> value = try_pop_optional();
        if (!value)
            return std::shared_ptr<T>();
        return std::make_shared<T>(std::move(*value));
    }
};

}
