// A finding of the review of 6 Nov 2026: std::allocator<Item> specialised
// as a class derived from the program's own bounding allocator: its
// allocate is the base's, found through allocator_traits. Lifted, the
// program printed otherwise; now the class is refused, and the program
// prints what it prints as written.
//
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.exe | FileCheck %s --match-full-lines
// RUN: %wtc_cc --lift -O1 -DWTC_CHECK -DDEQUE %s -o %t.deque.exe 2>&1 | FileCheck %s --check-prefix=WHY
// RUN: %t.deque.exe | FileCheck %s --match-full-lines
//
// CHECK: pushed 191 refused 809 popped 191
// WHY: locked queue q_ in Q not lifted: (e) the program's std::allocator<Item> allocates with an allocator other than libstdc++'s, which may bound or count what the container holds; the substitute allocates

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <queue>

enum class Item : long {};  // a program-defined type, held as a scalar
static int blocks = 0;
// The program's own bounding allocator, and std::allocator<Item>
// specialised as a class derived from it: its allocate is CapAlloc's, whose
// name is not std::allocator<Item>, so the check finds nothing to refuse.
template <class T> struct CapAlloc {
    using value_type = T;
    T* allocate(std::size_t n, const void* = nullptr) {
        if (++blocks > 3) throw std::bad_alloc();
        return static_cast<T*>(::operator new(n * sizeof(T)));
    }
    void deallocate(T* p, std::size_t) noexcept { --blocks; ::operator delete(p); }
    std::size_t max_size() const noexcept { return std::size_t(-1) / sizeof(T); }
};
namespace std {
template <> struct allocator<Item> : CapAlloc<Item> {
    using value_type = Item;
    using pointer = Item*;
    using const_pointer = const Item*;
    using reference = Item&;
    using const_reference = const Item&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;
    template <class U> struct rebind { using other = allocator<U>; };
    allocator() noexcept = default;
    template <class U> allocator(const allocator<U>&) noexcept {}
    template <class U, class... A> void construct(U* p, A&&... a) { ::new ((void*)p) U(std::forward<A>(a)...); }
    template <class U> void destroy(U* p) { p->~U(); }
    bool operator==(const allocator&) const noexcept { return true; }
    bool operator!=(const allocator&) const noexcept { return false; }
};
}  // namespace std

class Q {
#ifdef DEQUE
    std::deque<Item> q_;
#else
    std::queue<Item> q_;
#endif
    std::mutex m_;
    std::condition_variable cv_;

public:
    void push(Item v) {
#ifdef DEQUE
        { std::lock_guard<std::mutex> l(m_); q_.push_back(std::move(v)); }
#else
        { std::lock_guard<std::mutex> l(m_); q_.push(std::move(v)); }
#endif
        cv_.notify_one();
    }
    bool try_pop(Item& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
#ifdef DEQUE
        q_.pop_front();
#else
        q_.pop();
#endif
        return true;
    }
};

int main() {
    Q q;
    int refused = 0, pushed = 0;
    for (long i = 0; i < 1000; ++i) {
        try {
            q.push(static_cast<Item>(i));
            ++pushed;
        } catch (const std::bad_alloc&) {
            ++refused;
        }
    }
    Item v;
    long n = 0;
    while (q.try_pop(v)) ++n;
    std::printf("pushed %d refused %d popped %ld\n", pushed, refused, n);
}
