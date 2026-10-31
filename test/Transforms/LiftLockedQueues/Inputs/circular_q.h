// spdlog's circular_q (details/circular_q.h), as bounded.cpp writes it.
#pragma once
#include <cstddef>
#include <vector>

#define RING(Name, SLOTS, GUARD)                                            \
    template <typename T>                                                   \
    class Name {                                                            \
        size_t max_items_ = 0;                                              \
        size_t head_ = 0;                                                   \
        size_t tail_ = 0;                                                   \
        size_t overrun_counter_ = 0;                                        \
        std::vector<T> v_;                                                  \
                                                                            \
    public:                                                                 \
        using value_type = T;                                               \
        Name() = default;                                                   \
        explicit Name(size_t max_items) : max_items_(SLOTS), v_(max_items_) {} \
        void push_back(T &&item) {                                          \
            if (max_items_ > 0) {                                           \
                GUARD                                                       \
                v_[tail_] = std::move(item);                                \
                tail_ = (tail_ + 1) % max_items_;                           \
                if (tail_ == head_) {                                       \
                    head_ = (head_ + 1) % max_items_;                       \
                    ++overrun_counter_;                                     \
                }                                                           \
            }                                                               \
        }                                                                   \
        T &front() { return v_[head_]; }                                    \
        size_t size() const {                                               \
            if (tail_ >= head_) return tail_ - head_;                       \
            return max_items_ - (head_ - tail_);                            \
        }                                                                   \
        void pop_front() { head_ = (head_ + 1) % max_items_; }              \
        bool empty() const { return tail_ == head_; }                       \
        bool full() const {                                                 \
            if (max_items_ > 0) return ((tail_ + 1) % max_items_) == head_; \
            return false;                                                   \
        }                                                                   \
        size_t overrun_counter() const { return overrun_counter_; }         \
    };

RING(Ring, max_items + 1, )
