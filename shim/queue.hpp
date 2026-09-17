#pragma once

#include <memory>
#include <mutex>

namespace wtc {

template <typename T>
class queue {
private:
    struct node {
        std::shared_ptr<T> data;
        std::unique_ptr<node> next;
    };

    std::unique_ptr<node> head;
    node* tail;
    std::mutex head_mutex;
    std::mutex tail_mutex;

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

public:
    [[clang::annotate("wtc_queue_construction")]]
    queue() : head(new node), tail(head.get()) {}

    queue(const queue&) = delete;
    queue& operator=(const queue&) = delete;

    queue(queue&&) = operator delete;
    queue& operator=(queue&&) = delete;

    ~queue() {
        
    }

    [[clang::annotate("wtc_queue_try_pop")]]
    std::shared_ptr<T> try_pop() {
        std::unique_ptr<node> old_head = pop_head();
        return old_head ? old_head->data : std::shared_ptr<T>();
    }

    [[clang::annotate("wtc_queue_push")]]
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

}