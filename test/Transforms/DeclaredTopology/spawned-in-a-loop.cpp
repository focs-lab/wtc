// One spawn site, in a loop: one function, but certainly several threads.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK: error: wtc: queue @q is declared spsc but many threads push to it
// CHECK: note: push on a thread that starts in worker(), one of several started at

#include "queue.hpp"
#include "thread.hpp"

#include <vector>

wtc::queue<int> q(wtc::topology::spsc);

void worker() { q.push(1); }
void reader() { for (int got = 0; got < 4;) if (q.try_pop()) ++got; }

int main() {
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i) ts.push_back(wtc::spawn(worker));
    std::thread r = wtc::spawn(reader);
    for (auto& t : ts) t.join();
    r.join();
}
