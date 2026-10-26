// A declared spsc queue pushed to from two thread functions, one started
// on each arm of an if in a function that runs once: at most one of the two
// threads exists, so they count as one producer and the declaration is
// verified. With SETJMP, a longjmp back to a setjmp before the if runs it
// again and may take the other arm: the two are not exclusive, and since
// each is started under a condition of its own, the check warns.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: error:
// CHECK: queue @q: spsc (producers: 1, consumers: 1)
// CHECK: queue @q: declared spsc, verified
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DSETJMP -o %t.setjmp.cir
// RUN: wtc-opt %t.setjmp.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=SETJMP
// SETJMP-NOT: started on different arms of one branch
// SETJMP: warning: wtc: queue @q is declared spsc; cannot verify: the threads that push to it are each started under a condition of its own

#include "queue.hpp"
#include "thread.hpp"

#include <csetjmp>

std::jmp_buf env;

wtc::queue<int> q(wtc::topology::spsc);
int which;

void t1() { q.push(1); }
void t2() { q.push(2); }

int main() {
#ifdef SETJMP
    const int round = setjmp(env);
#endif
    std::thread t;
    if (which) t = wtc::spawn(t1);
    else t = wtc::spawn(t2);
    q.try_pop();
    t.join();
#ifdef SETJMP
    if (round == 0) {
        which = !which;
        std::longjmp(env, 1);
    }
#endif
}
