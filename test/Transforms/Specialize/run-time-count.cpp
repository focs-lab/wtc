// A side whose threads one counted loop starts, one per iteration, has one
// thread when the loop runs at most once, and the program knows whether it
// will before the loop runs: the constructor's tag becomes that choice.
// Consumers started by a loop over a global count, for a queue made on the
// heap (as PARSEC ferret's stages are); producers started by a loop over a
// local count, for a queue on the stack; and both sides counted.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o %t.wtc.cir 2>%t.remarks
// RUN: FileCheck %s < %t.remarks
// RUN: FileCheck %s --check-prefix=IR < %t.wtc.cir
// RUN: FileCheck %s --check-prefix=NONE < %t.remarks
//
// CHECK-DAG: spmc (producers: 1, consumers: many)
// CHECK-DAG: consumers: one thread per iteration of the loop at run-time-count.cpp:{{[0-9]+}}:{{[0-9]+}}, so one when it runs at most once
// CHECK-DAG: specialised at run time: spsc_inline when the loop at run-time-count.cpp:{{[0-9]+}}:{{[0-9]+}} (consumers) runs at most once, else spmc
// CHECK-DAG: queue %results: mpsc (producers: many, consumers: 1)
// CHECK-DAG: queue %results: producers: one thread per iteration of the loop at run-time-count.cpp:{{[0-9]+}}:{{[0-9]+}}, so one when it runs at most once
// CHECK-DAG: specialised at run time: spsc_inline when the loop at run-time-count.cpp:{{[0-9]+}}:{{[0-9]+}} (producers) runs at most once, else mpsc
// CHECK-DAG: mpmc (producers: many, consumers: many)
// CHECK-DAG: specialised at run time: spsc_inline when the loops at run-time-count.cpp:{{[0-9]+}}:{{[0-9]+}} (producers) and run-time-count.cpp:{{[0-9]+}}:{{[0-9]+}} (consumers) each run at most once, mpsc when only the second does, spmc when only the first does, else generic
// NONE-NOT: not specialised
//
// The count read where the queue is built, compared with the loop's start
// plus one, and the tag picked from it: spsc_inline (2) or spmc (4).
// IR-LABEL: cir.func {{.*}}@main
// IR: %[[N:.+]] = cir.load %{{.+}} : !cir.ptr<!s32i>, !s32i
// IR-NEXT: %[[ONE:.+]] = cir.const #cir.int<1> : !s32i
// IR-NEXT: %[[LE:.+]] = cir.cmp le %[[N]], %[[ONE]] : !s32i
// IR-NEXT: %[[INL:.+]] = cir.const #cir.int<2> : !s32i
// IR-NEXT: %[[SPMC:.+]] = cir.const #cir.int<4> : !s32i
// IR-NEXT: %[[TAG:.+]] = cir.select if %[[LE]] then %[[INL]] else %[[SPMC]]
// IR-NEXT: cir.call @_ZN3wtc5queueIiEC2ENS_8topologyE(%{{.+}}, %[[TAG]])

#include "queue.hpp"
#include "thread.hpp"

#include <cstdlib>
#include <thread>
#include <vector>

int NWORKERS = 1, NPROD = 1, NCONS = 1;
wtc::queue<int> *jobs;
wtc::queue<long> *mid;

void feed() { for (int i = 0; i < 100; ++i) jobs->push(i); }
void work() { for (int got = 0; got < 100 / NWORKERS;) if (jobs->try_pop()) ++got; }

int DRAIN = 10;
void fill(void *a) { for (short i = 0; i < 10; ++i) static_cast<wtc::queue<short> *>(a)->push(i); }
void drain(void *a) { for (int got = 0; got < DRAIN;) if (static_cast<wtc::queue<short> *>(a)->try_pop()) ++got; }

void put() { for (long i = 0; i < 10; ++i) mid->push(i); }
void take() { for (int got = 0; got < 10 * NPROD / NCONS;) if (mid->try_pop()) ++got; }

int main(int argc, char **argv) {
    NWORKERS = argc > 1 ? atoi(argv[1]) : 1;
    int nfill = argc > 2 ? atoi(argv[2]) : 1;
    DRAIN = 10 * nfill;
    NPROD = NCONS = argc > 3 ? atoi(argv[3]) : 1;
    jobs = new wtc::queue<int>();
    wtc::queue<short> results;
    mid = new wtc::queue<long>();
    std::vector<std::thread> pool;
    pool.push_back(wtc::spawn(feed));
    for (int i = 0; i < NWORKERS; i++) pool.push_back(wtc::spawn(work));
    for (int i = 0; i < nfill; i++) pool.push_back(wtc::spawn(fill, &results));
    pool.push_back(wtc::spawn(drain, &results));
    for (int i = 0; i < NPROD; i++) pool.push_back(wtc::spawn(put));
    for (int i = 0; i < NCONS; i++) pool.push_back(wtc::spawn(take));
    for (auto &t : pool) t.join();
}
