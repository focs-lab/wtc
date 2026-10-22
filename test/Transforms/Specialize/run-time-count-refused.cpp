// The tag is chosen when the queue is built only if the count read there is
// the one the loop will run with. Each queue here breaks one condition and
// is left to its static verdict, with the reason: built before main, by a
// global's constructor; the loop's bound written after the queue is built;
// written in another function; its address taken; the queue built after
// the loop; the loop's bound declared in a scope the constructor is
// outside of. A loop that may stop early does not start one thread per
// iteration and is not recorded.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o /dev/null 2>%t.remarks
// RUN: FileCheck %s < %t.remarks
// RUN: FileCheck %s --check-prefix=NONE < %t.remarks
//
// CHECK-DAG: not specialised at run time: the queue is built outside the function whose loop starts its threads
// CHECK-DAG: queue @global_q: spmc (producers: 1, consumers: many)
// CHECK-DAG: not specialised at run time: the loop's bound may be written after the queue is built
// CHECK-DAG: not specialised at run time: the loop's bound is written in another function
// CHECK-DAG: not specialised at run time: the loop's bound has its address taken
// CHECK-DAG: not specialised at run time: the queue is not built before the loop that starts its threads
// CHECK-DAG: not specialised at run time: the loop's bound is a variable the queue's constructor cannot see
// NONE-NOT: wtc: specialised at run time
// NONE-NOT: queue @early_q: consumers: one thread per iteration

#include "queue.hpp"
#include "thread.hpp"

#include <cstdlib>
#include <thread>
#include <vector>

int N1 = 1, N2 = 1, N3 = 1, N5 = 1, N6 = 1;
wtc::queue<int> global_q;
wtc::queue<long> *later_q;
wtc::queue<float> *after_q;
wtc::queue<double> *hidden_q;
void put6() { after_q->push(1); }
void take6() { after_q->try_pop(); }
void put7() { hidden_q->push(1); }
void take7() { hidden_q->try_pop(); }
wtc::queue<unsigned> *elsewhere_q;
wtc::queue<char> early_q;

void put1() { global_q.push(1); }
void take1() { global_q.try_pop(); }
void put2() { later_q->push(1); }
void take2() { later_q->try_pop(); }
void put3(void *q) { static_cast<wtc::queue<short> *>(q)->push(1); }
void take3(void *q) { static_cast<wtc::queue<short> *>(q)->try_pop(); }
void put5() { elsewhere_q->push(1); }
void take5() { elsewhere_q->try_pop(); }
void put4() { early_q.push(1); }
void take4() { early_q.try_pop(); }

void set5(int v) { N5 = v; }
void parse(int *n, const char *s) { *n = atoi(s); }

int main(int argc, char **argv) {
    std::vector<std::thread> pool;
    N1 = argc;
    pool.push_back(wtc::spawn(put1));
    for (int i = 0; i < N1; i++) pool.push_back(wtc::spawn(take1));

    later_q = new wtc::queue<long>();
    N2 = argc;
    pool.push_back(wtc::spawn(put2));
    for (int i = 0; i < N2; i++) pool.push_back(wtc::spawn(take2));

    int n3 = 1;
    parse(&n3, argv[0]);
    wtc::queue<short> taken_q;
    pool.push_back(wtc::spawn(put3, &taken_q));
    for (int i = 0; i < n3; i++) pool.push_back(wtc::spawn(take3, &taken_q));

    set5(argc);
    elsewhere_q = new wtc::queue<unsigned>();
    pool.push_back(wtc::spawn(put5));
    for (int i = 0; i < N5; i++) pool.push_back(wtc::spawn(take5));

    pool.push_back(wtc::spawn(put4));
    for (int i = 0; i < argc; i++) {
        if (i == 3) break;
        pool.push_back(wtc::spawn(take4));
    }

    N6 = argc;
    pool.push_back(wtc::spawn(put6));
    for (int i = 0; i < N6; i++) pool.push_back(wtc::spawn(take6));
    after_q = new wtc::queue<float>();

    hidden_q = new wtc::queue<double>();
    {
        int n7 = atoi(argv[0]);
        pool.push_back(wtc::spawn(put7));
        for (int i = 0; i < n7; i++) pool.push_back(wtc::spawn(take7));
    }
    for (auto &t : pool) t.join();
}
