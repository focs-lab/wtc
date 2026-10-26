// Threads started on the two arms of one branch, in a function that runs
// once and outside any loop: at most one of them exists, so they are one
// producer, as DPDK's packet_ordering starts either tx_thread or
// send_thread. Each refusal keeps both: the branch in a loop runs both arms,
// one per iteration; a function called twice may take a different arm each
// time; a spawn after the branch runs as well as the one inside it; a
// switch case that falls through runs the next case's spawn too; and a
// setjmp, which returns twice, lets one run of a function take both arms --
// in the function itself (SETJMP, and BUILTIN with __builtin_setjmp, which
// is an operation of its own rather than a call) or in a caller that jumps
// back to before the call (CALLER).
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DLOOP -o %t.loop.cir
// RUN: wtc-opt %t.loop.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=MANY
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DTWICE -o %t.twice.cir
// RUN: wtc-opt %t.twice.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=MANY
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DAFTER -o %t.after.cir
// RUN: wtc-opt %t.after.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=MANY
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DFALLTHROUGH -o %t.fall.cir
// RUN: wtc-opt %t.fall.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=MANY
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DSETJMP -o %t.setjmp.cir
// RUN: wtc-opt %t.setjmp.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefixes=MANY,NOSPSC
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DBUILTIN -o %t.builtin.cir
// RUN: wtc-opt %t.builtin.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefixes=MANY,NOSPSC
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DCALLER -o %t.caller.cir
// RUN: wtc-opt %t.caller.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefixes=MANY,NOSPSC
//
// CHECK: queue @q: spsc (producers: 1, consumers: 1)
// CHECK: queue @q: producers: 2 threads, started on different arms of one branch, so at most one exists
// CHECK: tag rewritten
// MANY: queue @q: mpsc (producers: many, consumers: 1)
// NOSPSC-NOT: rewritten to spsc

#include "queue.hpp"
#include "thread.hpp"

#include <csetjmp>

wtc::queue<int> q;
std::jmp_buf env;

void tx() { q.push(1); }
void send() { q.push(2); }

void start(bool reorder) {
#if defined(LOOP)
    for (int i = 0; i < 2; ++i) {
        std::thread t = reorder ? wtc::spawn(send) : wtc::spawn(tx);
        t.join();
        reorder = !reorder;
    }
#elif defined(AFTER)
    std::thread t;
    if (reorder) t = wtc::spawn(send);
    std::thread u = wtc::spawn(tx);
    if (t.joinable()) t.join();
    u.join();
#elif defined(FALLTHROUGH)
    std::thread t, u;
    switch (reorder) {
    case true: t = wtc::spawn(send); [[fallthrough]];
    case false: u = wtc::spawn(tx); break;
    }
    if (t.joinable()) t.join();
    u.join();
#elif defined(SETJMP)
    std::thread t;
    const int round = setjmp(env);
    if (round == 0) t = wtc::spawn(send);
    else t = wtc::spawn(tx);
    t.join();
    if (round == 0) std::longjmp(env, 1);
#elif defined(BUILTIN)
    static void *buf[5];
    std::thread t;
    const int round = __builtin_setjmp(buf);
    if (round == 0) t = wtc::spawn(send);
    else t = wtc::spawn(tx);
    t.join();
    if (round == 0) __builtin_longjmp(buf, 1);
#else
    std::thread t;
    if (reorder) t = wtc::spawn(send);
    else t = wtc::spawn(tx);
    t.join();
#endif
}

int main(int argc, char **) {
#ifdef CALLER
    const int round = setjmp(env);
    start(round == 0);
    if (round == 0) std::longjmp(env, 1);
#else
    start(argc > 1);
#endif
#ifdef TWICE
    start(argc > 2);
#endif
    q.try_pop();
}
