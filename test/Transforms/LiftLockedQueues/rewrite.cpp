// What the pass writes, with the runtime instantiated by hand the way
// wtc-cc instantiates it: the class's constructor builds the container
// through wtc::lifted<C>::construct with the generic tag, its push and
// try-pop become calls to the runtime's, which the lifting pass raises to
// wtc.queue_push and wtc.queue_pop on the container member; the ownership
// analysis finds one producer and one consumer, and the specialisation
// pass rewrites the tag at construct as it would at a wtc::queue's
// constructor: spsc_inline, since the element is trivially destructible
// and every pop moves it out. The runtime's forms the class does not use are erased.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-lift-locked-queues -o %t.lifted.cir 2>&1 | FileCheck %s --check-prefix=REMARK
// RUN: FileCheck %s < %t.lifted.cir
// RUN: wtc-opt %t.lifted.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis --wtc-specialize -o %t.spec.cir 2>&1 | FileCheck %s --check-prefix=SPEC
// RUN: FileCheck %s --check-prefix=OPS < %t.spec.cir
//
// REMARK: locked queue q_ in Box lifted (Box::push: push, Box::try_pop: try-pop)
//
// CHECK-NOT: @{{.*}}6lifted{{.*}}pop_wait
// CHECK-LABEL: cir.func {{.*}}@_ZN3BoxC2Ev(
// CHECK: %[[Q:.*]] = cir.get_member %{{.*}}[0] {name = "q_"}
// CHECK: %[[TAG:.*]] = cir.const #cir.int<0>
// CHECK: cir.call @{{.*}}6lifted{{.*}}9construct{{.*}}(%[[Q]], %[[TAG]])
// CHECK-LABEL: cir.func {{.*}}@_ZN3Box4pushEl(
// CHECK-NEXT: %[[AT:.*]] = cir.get_member %arg0[0] {name = "q_"}
// CHECK-NEXT: %[[SLOT:.*]] = cir.alloca "lifted.value"
// CHECK-NEXT: cir.store %arg1, %[[SLOT]]
// CHECK-NEXT: cir.call @{{.*}}6lifted{{.*}}9push_copy{{.*}}(%[[AT]], %[[SLOT]])
// CHECK: cir.return %{{.*}} : !cir.bool
// CHECK-LABEL: cir.func {{.*}}@_ZN3Box7try_popERl(
// CHECK-NEXT: %[[AT2:.*]] = cir.get_member %arg0[0] {name = "q_"}
// CHECK-NEXT: %[[R:.*]] = cir.call @{{.*}}6lifted{{.*}}7try_pop{{.*}}(%[[AT2]], %arg1)
// CHECK-NEXT: cir.return %[[R]] : !cir.bool
//
// SPEC: queue @box.q_: spsc (producers: 1, consumers: 1)
// SPEC: specialised: constructor tag rewritten to spsc_inline
//
// OPS: wtc.queue_push
// OPS: wtc.queue_pop

#include <mutex>
#include <queue>
#include <thread>

class Box {
public:
    bool push(long v) {
        std::lock_guard<std::mutex> l(m_);
        q_.push(v);
        return true;
    }
    bool try_pop(long& out) {
        std::lock_guard<std::mutex> l(m_);
        if (q_.empty()) return false;
        out = q_.front();
        q_.pop();
        return true;
    }

private:
    std::queue<long> q_;
    std::mutex m_;
};

Box box;
long got = 0;

int main() {
    std::thread consumer([] {
        long v, n = 0;
        while (n < 1000)
            if (box.try_pop(v)) got += v, ++n;
    });
    for (long i = 0; i < 1000; ++i) box.push(i);
    consumer.join();
    return got == 999 * 1000 / 2 ? 0 : 1;
}

#include "lifted.hpp"
template struct wtc::lifted<std::queue<long, std::deque<long, std::allocator<long>>>>;
