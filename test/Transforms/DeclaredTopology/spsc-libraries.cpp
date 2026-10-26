// The single-producer single-consumer queues of four libraries declare
// their topology through their type. One of each is used as declared and
// verified; one of each is used by a second thread on one side, and the
// build stops naming it. The type declares even when the constructor is
// out of sight (`rig_extern`, built in another file), and a queue of queues
// is the outer class's (`nested`).
//
// RUN: %clangxx -std=c++17 -I%S/Inputs -fclangir -emit-cir %s -o %t.cir
// RUN: not wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s
//
// CHECK-DAG: remark: wtc: queue @rig_ok: declared spsc (by its type), verified in this translation unit
// CHECK-DAG: error: wtc: queue @rig_bad is declared spsc (by its type) but 2 threads push to it
// CHECK-DAG: remark: wtc: queue @folly_ok: declared spsc (by its type), verified in this translation unit
// CHECK-DAG: error: wtc: queue @folly_bad is declared spsc (by its type) but 2 threads pop from it
// CHECK-DAG: remark: wtc: queue @boost_ok: declared spsc (by its type), verified in this translation unit
// CHECK-DAG: error: wtc: queue @boost_bad is declared spsc (by its type) but 2 threads push to it
// CHECK-DAG: remark: wtc: queue @moody_ok: declared spsc (by its type), verified in this translation unit
// CHECK-DAG: error: wtc: queue @moody_bad is declared spsc (by its type) but 2 threads pop from it
// CHECK-DAG: remark: wtc: queue @blocking_ok: declared spsc (by its type), verified in this translation unit
// CHECK-DAG: error: wtc: queue @rig_extern is declared spsc (by its type) but 2 threads push to it
// CHECK-DAG: remark: wtc: queue @nested: declared spsc (by its type), verified in this translation unit
// CHECK-NOT: is declared mpmc

#include "spsc_libs.hpp"
#include <thread>

rigtorp::SPSCQueue<int> rig_ok(16), rig_bad(16);
folly::ProducerConsumerQueue<int> folly_ok(16), folly_bad(16);
boost::lockfree::spsc_queue<int> boost_ok(16), boost_bad(16);
moodycamel::ReaderWriterQueue<int> moody_ok(16), moody_bad(16);
moodycamel::BlockingReaderWriterQueue<int> blocking_ok(16);
extern rigtorp::SPSCQueue<int> rig_extern;
rigtorp::SPSCQueue<moodycamel::ReaderWriterQueue<int> *> nested(4);

void producer() {
    rig_ok.push(1);
    rig_bad.push(1);
    folly_ok.write(1);
    folly_bad.write(1);
    boost_ok.push(1);
    boost_bad.push(1);
    moody_ok.enqueue(1);
    moody_bad.enqueue(1);
    blocking_ok.enqueue(1);
    rig_extern.push(1);
    nested.push(&moody_ok);
}

void consumer() {
    int v;
    if (rig_ok.front()) rig_ok.pop();
    if (rig_bad.front()) rig_bad.pop();
    folly_ok.read(v);
    folly_bad.read(v);
    boost_ok.pop(v);
    boost_bad.pop(v);
    moody_ok.try_dequeue(v);
    moody_bad.try_dequeue(v);
    blocking_ok.wait_dequeue(v);
    if (nested.front()) nested.pop();
}

void second_producer() {
    rig_bad.try_push(2);
    boost_bad.push(2);
    rig_extern.push(2);
}

void second_consumer() {
    int v;
    folly_bad.read(v);
    moody_bad.try_dequeue(v);
}

int main() {
    std::thread p(producer), c(consumer), p2(second_producer), c2(second_consumer);
    p.join(); c.join(); p2.join(); c2.join();
}
