// How many threads each side of a queue stands for, as a number where the
// program fixes one: a spawn in a loop of constant trip count stands for
// that many threads, nested loops for the product, a loop whose bound is
// a variable for that variable, and a spawn in a while loop for no number
// at all (as do a function whose address is taken and a goto). The counts
// sit on the queue's root beside its topology; the topology remarks are
// what they were.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --mlir-print-op-on-diagnostic=false -o - 2>&1 | FileCheck %s
//
// CHECK-DAG: remark: wtc: queue @four: mpsc (producers: many, consumers: 1)
// CHECK-DAG: cir.global {{.*}}@four = {{.*}}wtc.consumers_count = "1", wtc.producers_count = "4"
// CHECK-DAG: cir.global {{.*}}@six = {{.*}}wtc.consumers_count = "1", wtc.producers_count = "7"
// CHECK-DAG: cir.global {{.*}}@nested = {{.*}}wtc.consumers_count = "6", wtc.producers_count = "1"
// CHECK-DAG: cir.global {{.*}}@by_var = {{.*}}wtc.consumers_count = "1", wtc.producers_count = "workers"
// CHECK-DAG: cir.global {{.*}}@by_while = {{.*}}wtc.consumers_count = "1", wtc.producers_count = "unbounded"
// CHECK-DAG: cir.global {{.*}}@spsc = {{.*}}wtc.consumers_count = "1", wtc.producers_count = "1"

#include "queue.hpp"
#include "thread.hpp"

#include <thread>
#include <vector>

wtc::queue<int> four, six, nested, by_var, by_while, spsc;
int workers = 3;
int keep_going = 1;

#define SIDES(q) \
    void put_##q() { q.push(1); } \
    void take_##q() { q.try_pop(); }
SIDES(four) SIDES(six) SIDES(nested) SIDES(by_var) SIDES(by_while) SIDES(spsc)

int main() {
    std::vector<std::thread> ts;
    // Four producers, one consumer.
    for (int i = 0; i < 4; ++i) ts.push_back(wtc::spawn(put_four));
    ts.push_back(wtc::spawn(take_four));
    // Six from an inclusive loop, and one more from main itself.
    for (int i = 1; i <= 6; ++i) ts.push_back(wtc::spawn(put_six));
    six.push(2);
    ts.push_back(wtc::spawn(take_six));
    // Two times three consumers.
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 3; ++j) ts.push_back(wtc::spawn(take_nested));
    ts.push_back(wtc::spawn(put_nested));
    // As many producers as a variable says.
    for (int i = 0; i < workers; ++i) ts.push_back(wtc::spawn(put_by_var));
    ts.push_back(wtc::spawn(take_by_var));
    // As many as a condition lets through.
    int n = 0;
    while (keep_going && n++ < 8) ts.push_back(wtc::spawn(put_by_while));
    ts.push_back(wtc::spawn(take_by_while));
    ts.push_back(wtc::spawn(put_spsc));
    ts.push_back(wtc::spawn(take_spsc));
    for (auto &t : ts) t.join();
}
