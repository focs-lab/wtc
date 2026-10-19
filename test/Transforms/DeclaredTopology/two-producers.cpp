// The example the project's proposal opens its checks with: a queue the
// program declares single-producer, pushed to by two threads. The build
// stops and names both.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
// RUN: not %wtc_cc %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=BUILD
//
// BUILD: error: wtc: queue @q is declared spsc but 2 threads push to it
//
// CHECK: two-producers.cpp:18:17: error: wtc: queue @q is declared spsc but 2 threads push to it
// CHECK: two-producers.cpp:20:16: note: push on a thread that starts in task1() at {{.*}}two-producers.cpp:25:22
// CHECK: two-producers.cpp:21:16: note: push on a thread that starts in task2() at {{.*}}two-producers.cpp:26:22

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::spsc);  // the program promises one writer

void task1() { q.push(1); }
void task2() { q.push(2); }
void reader() { while (!q.try_pop()) {} }

int main() {
    std::thread t1 = wtc::spawn(task1);
    std::thread t2 = wtc::spawn(task2);
    std::thread r = wtc::spawn(reader);
    t1.join(); t2.join(); r.join();
}
