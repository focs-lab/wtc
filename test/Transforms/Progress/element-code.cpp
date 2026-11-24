// The element's own code runs inside a queue operation: a push runs its
// copy or move constructor, a pop that moves it out runs its
// move-assignment and destructor. The check follows them as calls the
// operation makes. A consumer of strings that transfers on the ring
// neither waits nor allocates in the ring, and fails on the string's
// move-assignment, which frees the buffer it held. A consumer of a
// trivially copyable struct, or of one whose members only count, runs
// nothing of the element's that blocks, and the ring's own steps neither
// wait nor allocate, so it passes. A member without a body here is
// opaque, as any call is.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --wtc-check-progress -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: specialised: constructor tag rewritten to spsc
// CHECK-LABEL: error: wtc: take_string(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>&) is declared lock-free but can block
// CHECK: note: runs the element's move-assignment, std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>::operator=(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>&&)
// CHECK: note: which calls the allocator
// CHECK-LABEL: remark: wtc: take_point(Point&) is declared lock-free: nothing it reaches blocks
// CHECK-LABEL: remark: wtc: take_counted(Counted&) is declared lock-free: nothing it reaches blocks
// CHECK-LABEL: error: wtc: take_opaque(Opaque&) is declared lock-free but can block
// CHECK: note: runs the element's move-assignment, Opaque::operator=(Opaque&&)
// CHECK: note: which calls Opaque::operator=(Opaque&&), whose body is not visible here, so it is counted as blocking

#include "progress.hpp"
#include "queue.hpp"
#include "thread.hpp"

#include <atomic>
#include <string>

struct Point {
    int x, y;
};

std::atomic<long> moves{0};
struct Counted {
    long v = 0;
    Counted() = default;
    Counted(const Counted &o) : v(o.v) { moves.fetch_add(1, std::memory_order_relaxed); }
    Counted &operator=(Counted &&o) noexcept {
        v = o.v;
        moves.fetch_add(1, std::memory_order_relaxed);
        return *this;
    }
    ~Counted() { moves.fetch_add(1, std::memory_order_relaxed); }
};

struct Opaque {
    long v = 0;
    Opaque() = default;
    Opaque(const Opaque &o);
    Opaque &operator=(Opaque &&o) noexcept;
};

wtc::queue<std::string> strings;
wtc::queue<Point> points;
wtc::queue<Counted> counted;
wtc::queue<Opaque> opaque;

WTC_LOCK_FREE bool take_string(std::string &s) { return strings.try_pop(s); }
WTC_LOCK_FREE bool take_point(Point &p) { return points.try_pop(p); }
WTC_LOCK_FREE bool take_counted(Counted &c) { return counted.try_pop(c); }
WTC_LOCK_FREE bool take_opaque(Opaque &o) { return opaque.try_pop(o); }

void producer() {
    strings.push(std::string(64, 'x'));
    points.push(Point{1, 2});
    counted.push(Counted{});
    opaque.push(Opaque{});
}

void consumer() {
    std::string s;
    Point p;
    Counted c;
    Opaque o;
    while (!take_string(s)) {}
    while (!take_point(p)) {}
    while (!take_counted(c)) {}
    while (!take_opaque(o)) {}
}

int main() {
    std::thread a = wtc::spawn(producer);
    std::thread b = wtc::spawn(consumer);
    a.join();
    b.join();
}
