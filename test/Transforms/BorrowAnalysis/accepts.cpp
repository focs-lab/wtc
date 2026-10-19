// Consumers that only borrow what they pop: each queue is marked, at -O0
// and at -O2, where the slot gains lifetime markers and a second cleanup
// scope.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis 2>&1 >/dev/null | FileCheck %s
// RUN: %clangxx -std=c++17 -O2 -I%shim -fclangir -emit-cir %s -o %t.o2.cir
// RUN: wtc-opt %t.o2.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis 2>&1 >/dev/null | FileCheck %s

#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>

struct record {
    std::uint64_t seq;
    std::uint64_t pad[7];
    std::uint64_t first() const { return pad[0]; }
};

wtc::queue<record> fields, member_call, reference, forward_a, forward_b;
std::uint64_t sum = 0;

void produce(wtc::queue<record>& q) {
    for (std::uint64_t i = 0; i < 100; ++i) q.push(record{i, {i}});
}

// Members read through ->, and a null test.
void read_fields() {
    for (int got = 0; got < 100;) {
        auto p = fields.try_pop();
        if (!p) continue;
        sum += p->seq + p->pad[3];
        ++got;
    }
}

// A member function of the element, which only reads it, and a copy of the
// whole element out.
void call_member() {
    for (int got = 0; got < 100;) {
        auto p = member_call.try_pop();
        if (p == nullptr) continue;
        record r = *p;
        sum += p->first() + r.seq;
        ++got;
    }
}

// A reference to the element held in a local.
void through_reference() {
    for (int got = 0; got < 100;) {
        auto p = reference.try_pop();
        if (!p) continue;
        const record& r = *p;
        sum += r.seq;
        ++got;
    }
}

// Pushing the element on to another queue copies it; that is not keeping it.
void forward() {
    for (int got = 0; got < 100;) {
        auto p = forward_a.try_pop();
        if (!p) continue;
        forward_b.push(*p);
        ++got;
    }
}

void drain_b() {
    for (int got = 0; got < 100;) {
        auto p = forward_b.try_pop();
        if (!p) continue;
        sum += p.get()->seq;
        ++got;
    }
}

void p_fields() { produce(fields); }
void p_member() { produce(member_call); }
void p_reference() { produce(reference); }
void p_forward() { produce(forward_a); }

int main() {
    std::thread t[] = {wtc::spawn(p_fields),    wtc::spawn(read_fields),
                       wtc::spawn(p_member),    wtc::spawn(call_member),
                       wtc::spawn(p_reference), wtc::spawn(through_reference),
                       wtc::spawn(p_forward),   wtc::spawn(forward),
                       wtc::spawn(drain_b)};
    for (auto& x : t) x.join();
    return static_cast<int>(sum & 1);
}

// CHECK-DAG: queue @fields: every pop only borrows its element (1 pop)
// CHECK-DAG: queue @member_call: every pop only borrows its element (1 pop)
// CHECK-DAG: queue @reference: every pop only borrows its element (1 pop)
// CHECK-DAG: queue @forward_a: every pop only borrows its element (1 pop)
// CHECK-DAG: queue @forward_b: every pop only borrows its element (1 pop)
