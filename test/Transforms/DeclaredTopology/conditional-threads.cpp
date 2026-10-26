// A declared spsc queue pushed to from two threads, each started under an
// if of its own on one flag with opposite senses, as DPDK's distributor
// sample starts its distributor lcore or its combined rx-and-distributor
// lcore: the two are exclusive by a condition the walk does not compare,
// so there is no proof of two producers and the build goes on, with a
// warning. The count still says many: the queue is not specialised as
// single-producer. With ONE, one thread is started unconditionally and the
// other under the flag: both exist whenever the flag is set, and the
// declaration is broken. With SAME, both threads are started under ifs on
// one local with the same sense and no write between: both start or
// neither, and the declaration is broken too. With WRITTEN, the local is
// written between the two tests; with RETURN, the first arm returns, so
// the second test may not run; with NARROW, the second test reads a
// narrowing of the local, `(char)n`, which may be false when n is not:
// in each the build goes on with the warning.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DONE -o %t.one.cir
// RUN: not wtc-opt %t.one.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=ONE
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DSAME -o %t.same.cir
// RUN: not wtc-opt %t.same.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=ONE
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DWRITTEN -o %t.written.cir
// RUN: wtc-opt %t.written.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DRETURN -o %t.return.cir
// RUN: wtc-opt %t.return.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DNARROW -o %t.narrow.cir
// RUN: wtc-opt %t.narrow.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-NOT: error:
// CHECK: queue @q: mpsc (producers: many, consumers: 1)
// CHECK: warning: wtc: queue @q is declared spsc; cannot verify: the threads that push to it are each started under a condition of its own, and whether the conditions can hold together is not known
// ONE: error: wtc: queue @q is declared spsc but 2 threads push to it

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q(wtc::topology::spsc);
int flag;

void t1() { q.push(1); }
void t2() { q.push(2); }

int main(int argc, char **) {
    std::thread a, b;
#if defined(ONE)
    a = wtc::spawn(t1);
    if (flag) b = wtc::spawn(t2);
#elif defined(SAME) || defined(WRITTEN) || defined(RETURN) || defined(NARROW)
    int n = argc > 1 ? 256 : 0;
#ifdef RETURN
    if (n) {
        a = wtc::spawn(t1);
        q.try_pop();
        a.join();
        return 0;
    }
#else
    if (n) a = wtc::spawn(t1);
#endif
#ifdef WRITTEN
    n = flag;
#endif
#ifdef NARROW
    if ((char)n) b = wtc::spawn(t2);
#else
    if (n) b = wtc::spawn(t2);
#endif
#else
    if (!flag) a = wtc::spawn(t1);
    if (flag) b = wtc::spawn(t2);
#endif
    q.try_pop();
    if (a.joinable()) a.join();
    if (b.joinable()) b.join();
}
