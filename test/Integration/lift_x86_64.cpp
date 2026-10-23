// The hidden return slot on x86-64, whatever the host. try_pop returns a
// shared_ptr by value, which the x86-64 convention passes through a hidden
// first pointer: the pop's queue is its second operand, and its result is
// written through the first. Reading that shape wrong is how the
// prototype crashed on its first real try_pop, and a host with another
// convention would never exercise it, so this test emits for x86-64
// explicitly, lifts, analyses, specialises, lowers and translates to LLVM
// IR.
//
// REQUIRES: x86_64-cir
// RUN: %cir_x86_64_cxx -std=c++17 -I%shim -fclangir -emit-cir %examples/queue_example.cpp -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis 2>&1 >%t.wtc.cir | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.wtc.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize --lower-wtc-to-cir --reconcile-unrealized-casts -o %t.low.cir
// RUN: cir-translate --cir-to-llvmir %t.low.cir -o - | FileCheck %s --check-prefix=LL
//
// CHECK-DAG: queue @q1: spsc (producers: 1, consumers: 1)
// CHECK-DAG: queue @q2: mpmc (producers: many, consumers: many)
//
// The pop of q1 takes the queue, then the slot its result is written to,
// and keeps the slot's sret marker for lowering.
// IR: "wtc.queue_pop"(%{{[0-9]+}}, %{{[0-9]+}}) <{fallback_impl = @{{[^,]*}}try_pop{{[^,]*}}, {{.*}}source_arg_attrs = [{{{[^]]*}}llvm.sret
//
// LL: call void @_ZN3wtc5queueI4ItemE7try_popEv(ptr {{.*}}sret
