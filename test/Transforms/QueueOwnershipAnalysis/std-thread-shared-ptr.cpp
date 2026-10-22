// A queue that is a member of an object reached through pointers the walk
// cannot follow: the worker's `this` comes out of the tuple inside the
// std::thread state, the producer's out of a weak_ptr. The member is still
// named by its record type and offset, and that names a queue: the one
// its constructor builds in every object of the record. Both sides are
// counted on it, and the verdict is carried by that constructor call.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: unresolvable
// CHECK-DAG: queue q in any pool: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue work in any pair: mpmc (producers: many, consumers: many)
// CHECK-DAG: queue work in any pair: many consumers: 2 threads
// CHECK-DAG: specialised: constructor tag rewritten to spsc
// CHECK-NOT: unresolvable

#include "queue.hpp"

#include <memory>
#include <thread>

struct pool {
    wtc::queue<int> q;
    std::thread worker;
    int got = 0;
    pool() : worker(&pool::loop, this) {}
    ~pool() { worker.join(); }
    void loop() { while (got < 3) if (q.try_pop()) ++got; }
    void post(int v) { q.push(v); }
};

// Two objects of one record, each with one worker popping and two threads
// pushing: the member stands for both objects at once, so the count is
// the union -- two consumers, though each object has one. The union is
// at least each object's own threads, so a verdict over it holds for
// each object; what is given up is precision, never the other way round.
// (A loop record is the exception: it is read per object, at its own
// constructor, so a thread carries one only when that constructor run
// started it. See loop-of-another-object.cpp.)
struct pair {
    wtc::queue<long> work;
    std::thread worker;
    long sum = 0;
    pair() : worker(&pair::drain, this) {}
    ~pair() { worker.join(); }
    void drain() { for (int got = 0; got < 2;) if (auto p = work.try_pop()) { sum += *p; ++got; } }
    void feed(long v) { work.push(v); }
};

int main() {
    auto p = std::make_shared<pool>();
    std::weak_ptr<pool> w = p;
    for (int i = 0; i < 3; ++i)
        if (auto s = w.lock()) s->post(i);
    auto a = std::make_shared<pair>(), b = std::make_shared<pair>();
    std::thread fa([&] { a->feed(1); });
    std::thread fb([&] { b->feed(2); });
    a->feed(3);
    b->feed(4);
    fa.join();
    fb.join();
}
