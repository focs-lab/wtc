// A thread whose entry is in another file is handed the queue: that thread
// pushes too, and the program has two producers where the analysis sees
// one. The queue escapes and keeps the general-purpose implementation;
// specialised to the single-producer ring, the contract checks of
// WTC_CHECK stopped the program at the second producer.
//
// RUN: %clangxx -std=c++17 -I%shim -O1 -DWTC_CHECK -c %S/Inputs/spawn_elsewhere_other.cpp -o %t.other.o
// RUN: %wtc_cc -DWTC_CHECK %s %t.other.o -o %t.exe 2>&1 | FileCheck %s --check-prefix=BUILD
// RUN: %t.exe | FileCheck %s
//
// BUILD: remark: wtc: a queue escapes this translation unit (it is handed, with the address of push_elsewhere(void*), whose body is not here, to wtc::spawn(void (*)(void*), void*))
// BUILD: remark: wtc: queue @_ZL1q: unknown
// BUILD-NOT: rewritten
// CHECK: got 200000

#include "queue.hpp"
#include "thread.hpp"

#include <cstdio>

static wtc::queue<int> q;

void push_elsewhere(void* q);   // in the other file

static void producer() { for (int i = 0; i < 100000; ++i) q.push(i); }

int main() {
    auto u = wtc::spawn(push_elsewhere, &q);
    auto t = wtc::spawn(producer);
    int got = 0;
    while (got < 200000)
        if (q.try_pop()) ++got;
    t.join();
    u.join();
    std::printf("got %d\n", got);
}
