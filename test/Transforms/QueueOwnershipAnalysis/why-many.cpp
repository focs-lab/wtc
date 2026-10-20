// A count of many says why, in the program's terms, so that a programmer
// can see what to change for a smaller one: a spawn in a loop, threads
// started from a function nothing in this translation unit calls (found on
// a port of PARSEC dedup, whose threads start in Encode, called from main
// in another file), a function whose address is taken, a spawn under
// recursion, and two threads. A queue with one thread on each side needs no
// account.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>%t.remarks
// RUN: FileCheck %s < %t.remarks
// RUN: FileCheck %s --check-prefix=SINGLE < %t.remarks
//
// CHECK-DAG: queue @looped: mpsc (producers: many, consumers: 1)
// CHECK-DAG: queue @looped: many producers: started in a loop at why-many.cpp:{{[0-9]+}}:{{[0-9]+}}
// CHECK-DAG: queue @external: mpmc (producers: many, consumers: many)
// CHECK-DAG: queue @external: many producers: started from start_external(), which may run any number of times: nothing in this translation unit calls it
// CHECK-DAG: queue @external: many consumers: started from start_external(), which may run any number of times: nothing in this translation unit calls it
// CHECK-DAG: queue @two: mpsc (producers: many, consumers: 1)
// CHECK-DAG: queue @two: many producers: 2 threads: fill_two() started at why-many.cpp:{{[0-9]+}}:{{[0-9]+}}, fill_two() started at why-many.cpp:{{[0-9]+}}:{{[0-9]+}}
// CHECK-DAG: queue @taken: mpsc (producers: many, consumers: 1)
// CHECK-DAG: queue @taken: many producers: fill_taken() may run any number of times: its address is taken, so it may run on any thread
// CHECK-DAG: queue @recursive: mpsc (producers: many, consumers: 1)
// CHECK-DAG: queue @recursive: many producers: spawner(int) is reached again through recursion
// CHECK-DAG: queue @single: spsc (producers: 1, consumers: 1)
// SINGLE-NOT: queue @single: many

#include "queue.hpp"
#include "thread.hpp"

#include <thread>
#include <vector>

wtc::queue<int> looped, external, two, taken, recursive, single;

void fill_looped() { looped.push(1); }
void drain_looped() { for (int got = 0; got < 4;) if (looped.try_pop()) ++got; }

void fill_external() { external.push(1); }
void drain_external() { while (!external.try_pop()) {} }

void fill_two() { two.push(1); }
void drain_two() { for (int got = 0; got < 2;) if (two.try_pop()) ++got; }

void fill_taken() { taken.push(1); }
void drain_taken() { while (!taken.try_pop()) {} }
void (*hook)();

void fill_recursive() { recursive.push(1); }
void drain_recursive() { for (int got = 0; got < 2;) if (recursive.try_pop()) ++got; }
void spawner(int n) {
    if (n == 0) return;
    std::thread t = wtc::spawn(fill_recursive);
    spawner(n - 1);
    t.join();
}

void fill_single() { single.push(1); }
void drain_single() { while (!single.try_pop()) {} }

// Called, if at all, from another file: as many times as that file likes.
void start_external() {
    std::thread p = wtc::spawn(fill_external);
    std::thread c = wtc::spawn(drain_external);
    p.join();
    c.join();
}

int main() {
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) ts.push_back(wtc::spawn(fill_looped));
    ts.push_back(wtc::spawn(drain_looped));
    ts.push_back(wtc::spawn(fill_two));
    ts.push_back(wtc::spawn(fill_two));
    ts.push_back(wtc::spawn(drain_two));
    hook = fill_taken;
    ts.push_back(wtc::spawn(drain_taken));
    ts.push_back(wtc::spawn(drain_recursive));
    spawner(2);
    ts.push_back(wtc::spawn(fill_single));
    ts.push_back(wtc::spawn(drain_single));
    for (auto &t : ts) t.join();
}
