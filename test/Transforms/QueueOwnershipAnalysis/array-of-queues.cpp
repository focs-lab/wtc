// An array of queues reached with an index the analysis cannot read is
// counted as one queue: every element, with the union of the threads of
// all of them, so a verdict over the union holds for each element. The one
// constructor call that builds the elements, run once per element, carries
// the verdict to all of them.
//
// With ANY, a thread pushes to the element `which` names, main pops element 0:
// one producer and one consumer over all elements, so every element can be
// a single-producer single-consumer queue.
// With TWO, two threads push, each to the element its own index names: over
// the union that is two producers, and every element becomes the queue for
// many producers; telling the elements apart is what the union gives up.
// With CONST, only constant indices, and still one constructor in a loop builds
// every element of the global array: the elements are counted together as
// well, and the verdict reaches every one of them through that call.
// With MEMBER, a record holds a queue and an array of queues of another
// type: the array's elements are counted together, the other member stays
// a queue of its own, and each constructor gets its own verdict.
// With MIXED, one heap block holds two arrays of queues of different types,
// each built in a loop and read at indices the analysis cannot read: the
// block's elements have no one type: every access is refused, as an element
// of an array of unknown queues, and nothing is specialised.
// With ONCE, a queue built by placement new in a function that runs once,
// through a pointer parameter, is one queue and not an array: two threads
// push to a queue declared spsc, and the build fails.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DANY -o %t.any.cir
// RUN: wtc-opt %t.any.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=ANY
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DTWO -o %t.two.cir
// RUN: wtc-opt %t.two.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=TWO
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DCONST -o %t.const.cir
// RUN: wtc-opt %t.const.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=CONST
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DMEMBER -o %t.member.cir
// RUN: wtc-opt %t.member.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=MEMBER
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DMIXED -o %t.mixed.cir
// RUN: wtc-opt %t.mixed.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o /dev/null 2>&1 | FileCheck %s --check-prefix=MIXED
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DONCE -o %t.once.cir
// RUN: not wtc-opt %t.once.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=ONCE
//
// ANY: queue @qs[*]: spsc (producers: 1, consumers: 1)
// ANY: tag rewritten
// ANY-NOT: unresolvable
// TWO: queue @qs[*]: mpsc (producers: many, consumers: 1)
// TWO: tag rewritten to mpsc
// TWO-NOT: rewritten to spsc
// TWO-NOT: unresolvable
// CONST: queue @qs[*]: spsc (producers: 1, consumers: 1)
// CONST: tag rewritten
// CONST-NOT: unresolvable
// MEMBER-NOT: unresolvable
// MEMBER: queue @p[*]: spsc (producers: 1, consumers: 1)
// MEMBER: queue @p.in: spsc (producers: 1, consumers: 1)
// MEMBER-COUNT-2: tag rewritten to spsc
// ONCE: error: wtc: queue *@one is declared spsc but 2 threads push to it
// MIXED: unresolvable (the queue is an element of an array of unknown queues)
// MIXED-NOT: tag rewritten

#include "queue.hpp"
#include "thread.hpp"

#include <cstdlib>
#include <new>

wtc::queue<int> qs[2];
int which;

#ifdef ANY
void producer() { qs[which].push(1); }

int main() {
    std::thread t = wtc::spawn(producer);
    qs[0].try_pop();
    t.join();
}
#elif defined(TWO)
int other;
void first() { qs[which].push(1); }
void second() { qs[other].push(1); }

int main() {
    std::thread a = wtc::spawn(first);
    std::thread b = wtc::spawn(second);
    qs[which].try_pop();
    a.join();
    b.join();
}
#elif defined(MEMBER)
struct P {
    wtc::queue<int> in;
    wtc::queue<long> st[8];
} p;

void stage() {
    p.st[which].push(1);
    p.in.push(2);
}

int main() {
    std::thread t = wtc::spawn(stage);
    p.st[which].try_pop();
    p.in.try_pop();
    t.join();
}
#elif defined(MIXED)
constexpr int kN = 4;
wtc::queue<int> *ints;
wtc::queue<long> *longs;
void stage() {
    ints[which].push(1);
    longs[which].push(2);
}

int main() {
    ints = static_cast<wtc::queue<int> *>(std::malloc(kN * (sizeof(wtc::queue<int>) + sizeof(wtc::queue<long>))));
    longs = reinterpret_cast<wtc::queue<long> *>(ints + kN);
    for (int i = 0; i < kN; ++i) new (&ints[i]) wtc::queue<int>;
    for (int i = 0; i < kN; ++i) new (&longs[i]) wtc::queue<long>;
    std::thread t = wtc::spawn(stage);
    ints[which].try_pop();
    longs[which].try_pop();
    t.join();
}
#elif defined(ONCE)
wtc::queue<int> *one;
void init(wtc::queue<int> *q) { new (q) wtc::queue<int>(wtc::topology::spsc); }
void first() { one->push(1); }
void second() { one->push(2); }

int main() {
    one = static_cast<wtc::queue<int> *>(std::malloc(sizeof(wtc::queue<int>)));
    init(one);
    std::thread a = wtc::spawn(first);
    std::thread b = wtc::spawn(second);
    one->try_pop();
    a.join();
    b.join();
}
#else
void producer() { qs[1].push(1); }

int main() {
    std::thread t = wtc::spawn(producer);
    qs[1].try_pop();
    t.join();
}
#endif
