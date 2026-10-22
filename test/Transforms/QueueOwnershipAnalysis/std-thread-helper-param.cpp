// A member queue handed to helpers by reference, as the spdlog port's
// thread pool does: `put(q_, v)` from the logger, `take(q_, out)` from the
// worker. Inside the helpers the queue is a parameter, and the member it
// names is read per call site, through the instance's call chain, where
// the site's `this` cannot be placed -- the worker's out of the std::thread
// tuple, the logger's out of weak_ptr::lock(). One helper serves two
// records: its operation counts on the member each of its sites names.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: unresolvable
// CHECK-DAG: queue q_ in any pool: spmc (producers: 1, consumers: many)
// CHECK-DAG: queue q_ in any pool: consumers: one thread per iteration of the loop at std-thread-helper-param.cpp:{{[0-9]+}}:{{[0-9]+}}, so one when it runs at most once
// CHECK-DAG: queue q_ in any single: spsc (producers: 1, consumers: 1)
// CHECK-DAG: specialised at run time: {{spsc_inline|spsc}} when the loop at std-thread-helper-param.cpp:{{[0-9]+}}:{{[0-9]+}} (consumers) runs at most once, else spmc
// CHECK-NOT: unresolvable

#include "queue.hpp"

#include <functional>
#include <memory>
#include <thread>
#include <vector>

template <class T>
void put(wtc::queue<T> &q, T v) { q.push(v); }

template <class T>
bool take(wtc::queue<T> &q, T &out) {
    auto p = q.try_pop();
    if (!p) return false;
    out = *p;
    return true;
}

// The workers' lambda copies a std::function, as spdlog's does: the
// closure has a destructor, so the loop's body wraps the spawn in a
// cleanup scope, which must not hide that the spawn runs on every
// iteration.
struct pool {
    wtc::queue<int> q_;
    std::vector<std::thread> workers_;
    pool(int n, std::function<void()> on_start) {
        for (int i = 0; i < n; i++)
            workers_.emplace_back([this, on_start] { on_start(); this->loop(); });
    }
    ~pool() { for (auto &t : workers_) t.join(); }
    void loop() { int v; for (;;) if (take(q_, v) && v == 0) return; }
    void post(int v) { put(q_, v); }
};

struct single {
    wtc::queue<int> q_;
    std::thread worker_;
    single() : worker_([this] { int v; for (;;) if (take(q_, v) && v == 0) return; }) {}
    ~single() { worker_.join(); }
    void post(int v) { put(q_, v); }
};

int main(int argc, char **) {
    auto p = std::make_shared<pool>(argc, [] {});
    std::weak_ptr<pool> w = p;
    for (int i = 1; i <= 10; ++i)
        if (auto s = w.lock()) s->post(i);
    for (int i = 0; i < argc; ++i) p->post(0);
    auto one = std::make_shared<single>();
    std::weak_ptr<single> ws = one;
    if (auto s = ws.lock()) s->post(1);
    one->post(0);
}
