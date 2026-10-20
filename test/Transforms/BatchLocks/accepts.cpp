// A loop that takes and lets go of one mutex on every iteration, in a
// function marked WTC_BATCH, with nothing between the sections that could
// wait for another thread: the mutex is taken before the loop, each section
// ends in a step that lets it go every WTC_LOCK_BATCH_K sections, and it is
// let go after the loop. The same loop without the mark is left alone.
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %examples/lock_loop.cpp -o %t.cir
// RUN: wtc-opt %t.cir --lift-cir-to-wtc --wtc-batch-locks -o %t.out.cir 2>&1 | FileCheck %s
// RUN: FileCheck %s --check-prefix=IR < %t.out.cir
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir -DPLAIN %examples/lock_loop.cpp -o %t.plain.cir
// RUN: wtc-opt %t.plain.cir --lift-cir-to-wtc --wtc-batch-locks -o /dev/null 2>&1 | FileCheck %s --allow-empty --check-prefix=PLAIN
//
// CHECK: remark: wtc: critical sections in this loop batched: the mutex is let go every WTC_LOCK_BATCH_K (64) sections and at the end
//
// IR-LABEL: cir.func {{.*}}@_Z3addv()
// IR: cir.alloca "wtc.lock_batch"
// IR-NOT: "wtc.lock"
// IR: "wtc.lock_batch_begin"
// IR: cir.for
// IR-NOT: "wtc.lock"
// IR-NOT: "wtc.unlock"
// IR: "wtc.lock_batch_step"
// IR-NOT: "wtc.unlock"
// IR: "wtc.lock_batch_end"
// IR: cir.return
//
// PLAIN-NOT: remark: wtc: critical sections
