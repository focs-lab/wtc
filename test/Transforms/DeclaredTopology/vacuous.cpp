// A declaration holds vacuously on a side it declares single that nothing
// in this translation unit touches, and the report says so rather than
// calling it verified: nothing was checked there. A side that is seen is
// verified as before. vacuous-outcome=false gives the wording of before
// 29 Oct, which called both verified (VERIFIED).
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis=vacuous-outcome=false -o /dev/null 2>&1 | FileCheck %s --check-prefix=VERIFIED
//
// CHECK-DAG: remark: wtc: queue @pushed_only: declared spsc, verified for its producer and vacuously satisfied: no consumer access in this translation unit
// CHECK-DAG: remark: wtc: queue @fan_in: declared mpsc, vacuously satisfied: no consumer access in this translation unit
// CHECK-DAG: remark: wtc: queue @both: declared spsc, verified
// CHECK-NOT: error
//
// VERIFIED-DAG: remark: wtc: queue @pushed_only: declared spsc, verified
// VERIFIED-DAG: remark: wtc: queue @fan_in: declared mpsc, verified
// VERIFIED-NOT: vacuously

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> pushed_only(wtc::topology::spsc);
wtc::queue<int> fan_in(wtc::topology::mpsc);
wtc::queue<int> both(wtc::topology::spsc);

void first() {
    for (int i = 0; i < 10; ++i) {
        pushed_only.push(i);
        both.push(i);
        fan_in.push(i);
    }
}
void second() { for (int i = 0; i < 10; ++i) fan_in.push(i); }
void reader() { for (int got = 0; got < 10;) if (both.try_pop()) ++got; }

int main() {
    std::thread a = wtc::spawn(first), b = wtc::spawn(second), r = wtc::spawn(reader);
    a.join(); b.join(); r.join();
}
