// A queue that is a member of a struct is a queue of its own. Its verdict
// is kept on the one constructor call that builds it -- the struct's
// constructor, run here for this one object -- so rewriting that call's tag
// touches no other queue. The member is first in its struct, at the
// struct's own address: what tells the queue from the struct is what the
// constructor call builds there.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s
// RUN: %wtc_cc -DWTC_CHECK %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=CC
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
//
// CHECK-NOT: unresolvable
// CHECK: queue @c.q: spsc (producers: 1, consumers: 1)
// CHECK: specialised: constructor tag rewritten to spsc
// CC: queue @c.q: every pop only borrows its element
// CC: tag rewritten to spsc_inline
// OUT: tag 2 got 10

#include "queue.hpp"
#include "thread.hpp"

#include <cstdio>

struct channel {
    wtc::queue<int> q;
    int sent = 0;
};
channel c;

void producer() { for (int i = 0; i < 10; ++i) c.q.push(i); }

int main() {
    std::thread t = wtc::spawn(producer);
    int got = 0;
    while (got < 10)
        if (c.q.try_pop()) ++got;
    t.join();
    std::printf("tag %d got %d\n", static_cast<int>(c.q.tag()), got);
}
