// One allocation site in a loop makes a queue for each worker, declared
// spsc, and each worker pushes to its own. The analysis counts the threads
// of all the objects the site makes as one queue's, which is many
// producers; that is no proof that any one queue has two. The build goes
// on with a warning that says why it cannot verify.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: error:
// CHECK: warning: wtc: queue the heap object made at {{.*}} is declared spsc; cannot verify: it is one of the objects one allocation site makes

#include "queue.hpp"
#include "thread.hpp"

void worker(void* p) {
    auto* q = static_cast<wtc::queue<int>*>(p);
    for (int i = 0; i < 10; ++i) q->push(i);
}

int main() {
    std::thread ts[4];
    for (int i = 0; i < 4; ++i) {
        auto* q = new wtc::queue<int>(wtc::topology::spsc);
        ts[i] = wtc::spawn(worker, static_cast<void*>(q));
    }
    for (auto& t : ts) t.join();
}
