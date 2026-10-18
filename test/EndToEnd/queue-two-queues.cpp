// REQUIRES: clangir
// RUN: %cir_cxx %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null > %t.out
// RUN: FileCheck %s --check-prefix=Q1 < %t.out
// RUN: FileCheck %s --check-prefix=Q2 < %t.out

// The prototype's own example, recovered from git history (8597095^,
// examples/queue_example.cpp), now in the wtc namespace. It is real C++
// through the real frontend, where spawn and try_pop return by value
// through a hidden first argument. The right answer is known in advance:
// q1 has one producer and one consumer; q2 has two of each, since each of
// its functions is spawned twice.
//
// Each queue is checked by its own prefix, so the test does not depend on
// the order ClangIR emits functions in, while each verdict is still tied to
// the line naming its queue.

#include "queue.hpp"
#include "thread.hpp"

#include <memory>
#include <thread>

const int MAX_SIZE = 100;

struct Item {
    int val;
    Item(int val) : val(val) {}
};

wtc::queue<Item> q1;
wtc::queue<Item> q2;

void producer1() {
    for (int i = 0; i < MAX_SIZE; i++) {
        Item item(i);
        q1.push(item);
    }
}

void consumer1() {
    int i = MAX_SIZE;
    while (i) {
        std::shared_ptr<Item> ptr = q1.try_pop();
        if (ptr != nullptr)
            i--;
    }
}

void producer2() {
    for (int i = 0; i < MAX_SIZE; i++) {
        Item item(i);
        q2.push(item);
    }
}

void consumer2() {
    int i = MAX_SIZE;
    while (i) {
        std::shared_ptr<Item> ptr = q2.try_pop();
        if (ptr != nullptr)
            i--;
    }
}

int main() {
    std::thread t1 = wtc::spawn(producer1);
    std::thread t2 = wtc::spawn(consumer1);

    std::thread t3 = wtc::spawn(producer2);
    std::thread t4 = wtc::spawn(producer2);
    std::thread t5 = wtc::spawn(consumer2);
    std::thread t6 = wtc::spawn(consumer2);

    t1.join();
    t2.join();
    t3.join();
    t4.join();
    t5.join();
    t6.join();
}

// Q1-LABEL: Queue @q1
// Q1-NEXT: producers: 1
// Q1-NEXT: consumers: 1
// Q1-NEXT: classification: SPSC

// Q2-LABEL: Queue @q2
// Q2-NEXT: producers: 2
// Q2-NEXT: consumers: 2
// Q2-NEXT: classification: MPMC
