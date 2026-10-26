// The single-producer single-consumer queues the analysis models, with each
// library's namespace, class and method names (rigtorp/SPSCQueue.h,
// folly/ProducerConsumerQueue.h, boost/lockfree/spsc_queue.hpp,
// readerwriterqueue.h). The bodies are stand-ins. The analysis reads the
// calls as the queue's pushes and pops; it walks into a body only for the
// program's code the body calls (consume_one's functor), and the queue
// operations inside a body are the library's, not counted.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace detail {
template <typename T> struct Slots {
    T v[16];
    std::size_t head = 0, tail = 0;
    void put(const T &x) { v[head++ % 16] = x; }
    bool take(T &x) {
        if (tail == head) return false;
        x = v[tail++ % 16];
        return true;
    }
};
} // namespace detail

namespace rigtorp {
template <typename T, typename Allocator = std::allocator<T>> class SPSCQueue {
    detail::Slots<T> s;
public:
    explicit SPSCQueue(std::size_t) {}
    void push(const T &v) { s.put(v); }
    bool try_push(const T &v) { push(v); return true; }
    T *front() { return s.tail == s.head ? nullptr : &s.v[s.tail % 16]; }
    void pop() { ++s.tail; }
};
} // namespace rigtorp

namespace folly {
template <class T> struct ProducerConsumerQueue {
    detail::Slots<T> s;
    explicit ProducerConsumerQueue(std::uint32_t) {}
    template <class... Args> bool write(Args &&...a) { s.put(T(a...)); return true; }
    bool read(T &record) { return s.take(record); }
};
} // namespace folly

namespace boost::lockfree {
template <typename T> class spsc_queue {
    detail::Slots<T> s;
public:
    explicit spsc_queue(std::size_t) {}
    bool push(const T &t) { s.put(t); return true; }
    bool pop(T &ret) { return s.take(ret); }
    // The functor runs inside the library's body, on the consumer's thread.
    template <typename Functor> bool consume_one(Functor &&f) {
        T v;
        if (!s.take(v)) return false;
        f(v);
        return true;
    }
    template <typename Functor> std::size_t consume_all(Functor &&f) {
        std::size_t n = 0;
        while (consume_one(f)) ++n;
        return n;
    }
};
} // namespace boost::lockfree

namespace moodycamel {
template <typename T, std::size_t MAX_BLOCK_SIZE = 512> class ReaderWriterQueue {
    detail::Slots<T> s;
public:
    explicit ReaderWriterQueue(std::size_t = 15) {}
    bool enqueue(const T &element) { s.put(element); return true; }
    bool try_dequeue(T &result) { return s.take(result); }
};
template <typename T, std::size_t MAX_BLOCK_SIZE = 512> class BlockingReaderWriterQueue {
    ReaderWriterQueue<T, MAX_BLOCK_SIZE> inner;
public:
    explicit BlockingReaderWriterQueue(std::size_t size = 15) : inner(size) {}
    bool enqueue(const T &element) { return inner.enqueue(element); }
    void wait_dequeue(T &result) { while (!inner.try_dequeue(result)) {} }
};
} // namespace moodycamel
