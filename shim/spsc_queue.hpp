#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace wtc {


template <typename T, size_t N>
class spsc_queue {
    static_assert(N >= 1, "spsc_queue requires a non-zero capacity");

    static constexpr size_t kSlots = N + 1;

    std::array<std::optional<T>, kSlots> buffer_;

    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

    static constexpr size_t next(size_t idx) {
        size_t n = idx + 1;
        return n == kSlots ? 0 : n;
    }

public:
    [[clang::annotate("wtc_queue_construction")]]
    spsc_queue() {}

    spsc_queue(const spsc_queue&) = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;

    spsc_queue(spsc_queue&&) = delete;
    spsc_queue& operator=(spsc_queue&&) = delete;

    ~spsc_queue() {}

    std::optional<T> try_pop_optional() {
        const size_t head = head_.load(std::memory_order_relaxed);

        if (head == tail_.load(std::memory_order_acquire))
            return std::nullopt;

        std::optional<T> result = std::move(buffer_[head]);
        buffer_[head].reset();

        head_.store(next(head), std::memory_order_release);
        return result;
    }

    // ABI-matching wrapper: concur::queue<T>::try_pop() returns
    // shared_ptr<T>, and callers substituted from it (checking for null,
    // dereferencing) expect exactly that shape. This is the one automatic
    // substitution targets; try_pop_optional() above is for direct,
    // allocation-free use when spsc_queue is written by hand.
    [[clang::annotate("wtc_queue_try_pop")]]
    std::shared_ptr<T> try_pop() {
        auto value = try_pop_optional();
        if (!value)
            return std::shared_ptr<T>();
        return std::make_shared<T>(std::move(*value));
    }

    [[clang::annotate("wtc_queue_push")]]
    bool push(const T& new_value) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next_tail = next(tail);

        if (next_tail == head_.load(std::memory_order_acquire))
            return false;

        buffer_[tail].emplace(new_value);

        tail_.store(next_tail, std::memory_order_release);
        return true;
    }
};

}
