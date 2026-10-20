// Consumers that could notice the element is not theirs to keep. Each queue
// is spsc, so only the borrow is in question, and each is refused with the
// use that stands in the way.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-borrow-analysis 2>&1 >/dev/null | FileCheck %s

#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>
#include <memory>

struct record {
    std::uint64_t seq;
    const record* self() const;
};

const record* last = nullptr;
const record* record::self() const { last = this; return this; }
void opaque(const record*);

wtc::queue<record> copied, stored, popped_twice, external, counted, returned,
    called_popper, kept_by_member, reset_early, as_integer;
std::shared_ptr<record> keep;
std::uint64_t sum = 0;

void produce(wtc::queue<record>& q) {
    for (std::uint64_t i = 0; i < 100; ++i) q.push(record{i});
}

void c_copied() { auto p = copied.try_pop(); keep = p; }
void c_stored() { auto p = stored.try_pop(); if (p) last = p.get(); }
void c_popped_twice() {
    auto a = popped_twice.try_pop();
    auto b = popped_twice.try_pop();
    if (a && b) sum += a->seq + b->seq;
}
void c_external() { auto p = external.try_pop(); if (p) opaque(p.get()); }
void c_counted() { auto p = counted.try_pop(); sum += p.use_count(); }
const record* peek() { auto p = returned.try_pop(); return p.get(); }
void c_returned() { sum += peek() != nullptr; }
void drain() { called_popper.try_pop(); }
void c_called_popper() { auto p = called_popper.try_pop(); drain(); if (p) sum += p->seq; }
void c_kept_by_member() { auto p = kept_by_member.try_pop(); if (p) p->self(); }
// Harmless on an element held inline, but not on the list of what a
// borrower may do: the list is what is allowed, not what is known to hurt.
void c_reset_early() { auto p = reset_early.try_pop(); p.reset(); }
// An address turned into a number can be turned back after the next pop.
void c_as_integer() { auto p = as_integer.try_pop(); sum += reinterpret_cast<std::uintptr_t>(p.get()); }

void p1() { produce(copied); }
void p2() { produce(stored); }
void p3() { produce(popped_twice); }
void p4() { produce(external); }
void p5() { produce(counted); }
void p6() { produce(returned); }
void p7() { produce(called_popper); }
void p8() { produce(kept_by_member); }
void p9() { produce(reset_early); }
void p10() { produce(as_integer); }

int main() {
    std::thread t[] = {wtc::spawn(p1), wtc::spawn(c_copied),        wtc::spawn(p2),
                       wtc::spawn(c_stored),  wtc::spawn(p3),       wtc::spawn(c_popped_twice),
                       wtc::spawn(p4), wtc::spawn(c_external),      wtc::spawn(p5),
                       wtc::spawn(c_counted), wtc::spawn(p6),       wtc::spawn(c_returned),
                       wtc::spawn(p7), wtc::spawn(c_called_popper), wtc::spawn(p8),
                       wtc::spawn(c_kept_by_member), wtc::spawn(p9), wtc::spawn(c_reset_early),
                       wtc::spawn(p10), wtc::spawn(c_as_integer)};
    for (auto& x : t) x.join();
    return static_cast<int>(sum & 1);
}

// CHECK-DAG: queue @copied: not borrowed: the popped shared_ptr is passed to std::shared_ptr<record>::operator=
// CHECK-DAG: queue @stored: not borrowed: a pointer into the element is stored where it may outlive the borrow
// CHECK-DAG: queue @popped_twice: not borrowed: the same queue is popped again while the element is borrowed
// CHECK-DAG: queue @external: not borrowed: a pointer into the element is passed to opaque(record const*), whose body is not visible here
// CHECK-DAG: queue @counted: not borrowed: the popped shared_ptr is passed to std::__shared_ptr<record, (__gnu_cxx::_Lock_policy)2>::use_count() const
// CHECK-DAG: queue @returned: not borrowed: a pointer into the element is returned
// CHECK-DAG: queue @called_popper: not borrowed: a call to drain() may pop the same queue while the element is borrowed
// CHECK-DAG: queue @kept_by_member: not borrowed: a pointer into the element is passed to record::self() const, which keeps it or passes it on
// CHECK-DAG: queue @reset_early: not borrowed: the popped shared_ptr is passed to std::__shared_ptr<record, (__gnu_cxx::_Lock_policy)2>::reset()
// CHECK-DAG: queue @as_integer: not borrowed: a pointer into the element is converted to an integer
// CHECK-NOT: every pop only borrows
