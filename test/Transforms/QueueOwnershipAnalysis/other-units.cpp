// What other translation units may name. A unit with main is taken for the
// whole program, apart from libraries, which reach a queue only through
// what they are handed (escapes.cpp): a queue in a global with external
// linkage, one whose address is stored in such a global, one a function
// with external linkage hands out, and a function with external linkage
// called once here are all the program's own, and the queue is spsc
// (CLOSED). A unit built to be linked with other files of the program
// (open-world, which wtc-cc -c passes) shares every external symbol:
// each such queue escapes, and each such function is a root for any
// number of threads (OPEN). named-elsewhere shares only the symbols the
// other files name, which wtc-cc reads from them with nm (NAMED-EXTQ, and
// CLOSED for a symbol they do not name). A unit without main is not the
// program, and shares every external symbol unasked (MANY), unless
// named-elsewhere lists what the rest of the program names. A function
// with external linkage that nothing here calls counts many threads in
// either case (ORPHAN).
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DEXTERNAL_QUEUE -o %t.extq.cir
// RUN: wtc-opt %t.extq.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
// RUN: wtc-opt %t.extq.cir --lift-cir-to-wtc --queue-ownership-analysis=open-world=true -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPEN-EXTQ
// RUN: echo q > %t.names-q
// RUN: wtc-opt %t.extq.cir --lift-cir-to-wtc --queue-ownership-analysis=named-elsewhere=%t.names-q -o /dev/null 2>&1 | FileCheck %s --check-prefix=NAMED-EXTQ
// RUN: echo some_other_symbol > %t.names-other
// RUN: wtc-opt %t.extq.cir --lift-cir-to-wtc --queue-ownership-analysis=named-elsewhere=%t.names-other -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DSTORED -o %t.stored.cir
// RUN: wtc-opt %t.stored.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
// RUN: wtc-opt %t.stored.cir --lift-cir-to-wtc --queue-ownership-analysis=open-world=true -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPEN-STORED
// RUN: echo handle > %t.names-handle
// RUN: wtc-opt %t.stored.cir --lift-cir-to-wtc --queue-ownership-analysis=named-elsewhere=%t.names-handle -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPEN-STORED
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DTYPED -o %t.typed.cir
// RUN: wtc-opt %t.typed.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
// RUN: wtc-opt %t.typed.cir --lift-cir-to-wtc --queue-ownership-analysis=open-world=true -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPEN-TYPED
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DRETURNED -o %t.returned.cir
// RUN: wtc-opt %t.returned.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
// RUN: wtc-opt %t.returned.cir --lift-cir-to-wtc --queue-ownership-analysis=open-world=true -o /dev/null 2>&1 | FileCheck %s --check-prefix=OPEN-RETURNED
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DCALLED -o %t.called.cir
// RUN: wtc-opt %t.called.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
// RUN: wtc-opt %t.called.cir --lift-cir-to-wtc --queue-ownership-analysis=open-world=true -o /dev/null 2>&1 | FileCheck %s --check-prefix=MANY
// RUN: echo _Z7producei > %t.names-produce
// RUN: wtc-opt %t.called.cir --lift-cir-to-wtc --queue-ownership-analysis=named-elsewhere=%t.names-produce -o /dev/null 2>&1 | FileCheck %s --check-prefix=MANY
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DCALLED -DNO_MAIN -o %t.nomain.cir
// RUN: wtc-opt %t.nomain.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=MANY
// RUN: wtc-opt %t.nomain.cir --lift-cir-to-wtc --queue-ownership-analysis=named-elsewhere=%t.names-other -o /dev/null 2>&1 | FileCheck %s --check-prefix=CLOSED
// RUN: wtc-opt %t.nomain.cir --lift-cir-to-wtc --queue-ownership-analysis=named-elsewhere=%t.names-produce -o /dev/null 2>&1 | FileCheck %s --check-prefix=MANY
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DORPHAN -o %t.orphan.cir
// RUN: wtc-opt %t.orphan.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=ORPHAN
//
// RUN: not wtc-opt %t.called.cir --lift-cir-to-wtc --queue-ownership-analysis=named-elsewhere=%t.missing -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSING
//
// CLOSED-NOT: escapes this translation unit
// CLOSED: remark: wtc: queue @{{(_ZL1)?}}q: spsc (producers: 1, consumers: 1)
// CLOSED-NOT: many
//
// OPEN-EXTQ: remark: wtc: a queue escapes this translation unit (it is reachable from the global q, and the unit is built to be linked with other files of the program); every queue of its type is now unknown
// OPEN-EXTQ: remark: wtc: queue @q: unknown
// NAMED-EXTQ: remark: wtc: a queue escapes this translation unit (it is reachable from the global q, and another file of the program names it)
// NAMED-EXTQ: remark: wtc: queue @q: unknown
// OPEN-STORED: remark: wtc: a queue escapes this translation unit (its address is stored in the global handle, and
// OPEN-STORED: remark: wtc: queue @_ZL1q: unknown
// OPEN-TYPED: remark: wtc: a queue escapes this translation unit (it is reachable from the global shared_queue, and
// OPEN-TYPED: remark: wtc: queue @_ZL1q: unknown
// OPEN-RETURNED: remark: wtc: a queue escapes this translation unit (it is handed out by the_queue(), and
// OPEN-RETURNED: remark: wtc: queue @_ZL1q: unknown
//
// MANY-NOT: escapes this translation unit
// MANY: remark: wtc: queue @_ZL1q: mpsc (producers: many, consumers: 1)
// MANY: remark: wtc: queue @_ZL1q: many producers: produce(int) may run any number of times: another translation unit may call it
//
// ORPHAN: remark: wtc: queue @_ZL1q: mpsc (producers: many, consumers: 1)
// ORPHAN: remark: wtc: queue @_ZL1q: many producers: produce_more() may run any number of times: nothing in this translation unit calls it
//
// MISSING: error: wtc: cannot read the symbols named elsewhere from

#include "queue.hpp"
#include "thread.hpp"

#if defined(EXTERNAL_QUEUE)
wtc::queue<int> q;   // external linkage: another file may declare it extern
#else
static wtc::queue<int> q;
#endif

#if defined(STORED)
void* handle;   // another file may read it
#elif defined(TYPED)
wtc::queue<int>* shared_queue = &q;
#elif defined(RETURNED)
wtc::queue<int>& the_queue() { return q; }
#endif

#if defined(CALLED) || defined(ORPHAN)
void produce(int n) { for (int i = 0; i < n; ++i) q.push(i); }
static void producer() { produce(100); }
#else
static void producer() { for (int i = 0; i < 100; ++i) q.push(i); }
#endif

#if defined(ORPHAN)
void produce_more() { q.push(-1); }   // called from nowhere in this file
#endif

static int run() {
#if defined(STORED)
    handle = &q;
#endif
    auto t = wtc::spawn(producer);
    int got = 0;
    while (got < 100)
        if (q.try_pop()) ++got;
    t.join();
    return got;
}

#if defined(NO_MAIN)
static int ran = run();   // the program's main is in another file
#else
int main() { return run() == 100 ? 0 : 1; }
#endif
