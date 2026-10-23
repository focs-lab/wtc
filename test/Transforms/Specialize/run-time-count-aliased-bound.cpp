// A global bound whose address travels as a global_view: a pointer
// initialised with &N is a #cir.global_view<@N> in its initialiser, not a
// cir.get_global, so a store through that pointer after the queue is built
// is one the walk over get_global never sees. The bound counts as having
// its address taken, and the queue keeps its static verdict.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>%t.remarks
// RUN: FileCheck %s < %t.remarks
//
// CHECK-DAG: queue *@q: spmc (producers: 1, consumers: many)
// CHECK-DAG: not specialised at run time: the loop's bound has its address taken
// CHECK-NOT: wtc: specialised at run time

#include "queue.hpp"
#include "thread.hpp"

#include <cstdlib>
#include <thread>
#include <vector>

int N = 1;
int *pn = &N;
wtc::queue<int> *q;

void put() { q->push(1); }
void take() { q->try_pop(); }

int main(int argc, char **argv) {
    std::vector<std::thread> pool;
    N = argc;
    q = new wtc::queue<int>();
    pool.push_back(wtc::spawn(put));
    *pn = 2;  // the bound changes through the alias, after the queue is built
    for (int i = 0; i < N; i++) pool.push_back(wtc::spawn(take));
    for (auto &t : pool) t.join();
    return 0;
}
