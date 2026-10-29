// A queue whose address goes to a function with no body in this
// translation unit escapes it: the function is in another file of the
// program, or in a library, and may push to or pop from the queue on
// threads the walk never sees. The verdict on it was spsc, counted from
// the two threads in sight; it is unknown now, and every queue of its type
// keeps the general-purpose implementation. The queue may be handed over
// by pointer (POINTER), by reference (REFERENCE), inside a structure of
// parameters (STRUCT) or one passed by value (BYVALUE), cast to void *
// (VOID) or to an integer (INTEGER), through a function here that hands
// its void * parameter on (RELAY), in a void * field of a structure
// (FIELD) or of a heap object (HEAPBOX), stored through a pointer
// parameter (PARAMSTORE), along a chain of copies written in the reverse
// of program order (CHAIN), to a thread that starts in such a function -- wtc::spawn
// (SPAWN), std::thread (STDTHREAD), pthread_create (PTHREAD) -- or
// through a pointer to one (FNPTR). A queue only declared here is defined
// in another file, which names it (EXTERN). Handed to a function whose
// body is here, it stays in sight (KEPT).
//
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DPOINTER -o %t.pointer.cir
// RUN: wtc-opt %t.pointer.cir --lift-cir-to-wtc --queue-ownership-analysis --wtc-specialize -o %t.pointer.wtc.cir 2>&1 | FileCheck %s --check-prefix=POINTER
// RUN: FileCheck %s --check-prefix=GENERIC < %t.pointer.wtc.cir
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DREFERENCE -o %t.reference.cir
// RUN: wtc-opt %t.reference.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=REFERENCE
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DSTRUCT -o %t.struct.cir
// RUN: wtc-opt %t.struct.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=STRUCT
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DVOID -o %t.void.cir
// RUN: wtc-opt %t.void.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=VOID
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DBYVALUE -o %t.byvalue.cir
// RUN: wtc-opt %t.byvalue.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=BYVALUE
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DINTEGER -o %t.integer.cir
// RUN: wtc-opt %t.integer.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=INTEGER
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DRELAY -o %t.relay.cir
// RUN: wtc-opt %t.relay.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=RELAY
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DHEAPBOX -o %t.heapbox.cir
// RUN: wtc-opt %t.heapbox.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=HEAPBOX
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DPARAMSTORE -o %t.paramstore.cir
// RUN: wtc-opt %t.paramstore.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=PARAMSTORE
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DCHAIN -o %t.chain.cir
// RUN: wtc-opt %t.chain.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=CHAIN
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DFIELD -o %t.field.cir
// RUN: wtc-opt %t.field.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=FIELD
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DSPAWN -o %t.spawn.cir
// RUN: wtc-opt %t.spawn.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=SPAWN
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DSTDTHREAD -o %t.stdthread.cir
// RUN: wtc-opt %t.stdthread.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=STDTHREAD
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DPTHREAD -o %t.pthread.cir
// RUN: wtc-opt %t.pthread.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=PTHREAD
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DFNPTR -o %t.fnptr.cir
// RUN: wtc-opt %t.fnptr.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=FNPTR
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DEXTERN -o %t.extern.cir
// RUN: wtc-opt %t.extern.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=EXTERN
// RUN: %clangxx -std=c++17 -I%shim -fclangir -emit-cir %s -DKEPT -o %t.kept.cir
// RUN: wtc-opt %t.kept.cir --lift-cir-to-wtc --queue-ownership-analysis -o /dev/null 2>&1 | FileCheck %s --check-prefix=KEPT
//
// POINTER: remark: wtc: a queue escapes this translation unit (it is handed to hand_off(wtc::queue<int>*), whose body is not here); every queue of its type is now unknown
// POINTER: remark: wtc: queue @_ZL1q: unknown (producers: 1, consumers: 1)
// POINTER-NOT: rewritten
// GENERIC-NOT: wtc.specialized
// REFERENCE: remark: wtc: a queue escapes this translation unit (it is handed to hand_off_ref(wtc::queue<int>&), whose body is not here)
// REFERENCE: remark: wtc: queue @_ZL1q: unknown (producers: 1, consumers: 1)
// STRUCT: remark: wtc: a queue escapes this translation unit (it is handed to start_elsewhere(params*), whose body is not here)
// STRUCT: remark: wtc: queue @_ZL1q: unknown (producers: 1, consumers: 1)
// VOID: remark: wtc: a queue escapes this translation unit (it is handed to register_handle, whose body is not here)
// VOID: remark: wtc: queue @_ZL1q: unknown (producers: 1, consumers: 1)
// BYVALUE: remark: wtc: a queue escapes this translation unit (it is handed to start_by_value(params), whose body is not here)
// BYVALUE: remark: wtc: queue @_ZL1q: unknown
// INTEGER: remark: wtc: a queue escapes this translation unit (it is handed to register_word, whose body is not here)
// INTEGER: remark: wtc: queue @_ZL1q: unknown
// RELAY: remark: wtc: a queue escapes this translation unit (it is handed to register_handle, whose body is not here)
// RELAY: remark: wtc: queue @_ZL1q: unknown
// HEAPBOX: remark: wtc: a queue escapes this translation unit (it is handed to register_handle, whose body is not here)
// HEAPBOX: remark: wtc: queue @_ZL1q: unknown
// PARAMSTORE: remark: wtc: a queue escapes this translation unit (it is handed to register_handle, whose body is not here)
// PARAMSTORE: remark: wtc: queue @_ZL1q: unknown
// CHAIN: remark: wtc: a queue escapes this translation unit (it is handed to register_handle, whose body is not here)
// CHAIN: remark: wtc: queue @_ZL1q: unknown
// FIELD: remark: wtc: a queue escapes this translation unit (it is handed to start_context(context*), whose body is not here)
// FIELD: remark: wtc: queue @_ZL1q: unknown
// SPAWN: remark: wtc: a queue escapes this translation unit (it is handed, with the address of push_elsewhere(void*), whose body is not here, to wtc::spawn(void (*)(void*), void*))
// SPAWN: remark: wtc: queue @_ZL1q: unknown
// STDTHREAD: remark: wtc: a queue escapes this translation unit (it is handed, with the address of push_elsewhere_typed(wtc::queue<int>*), whose body is not here, to
// STDTHREAD: remark: wtc: queue @_ZL1q: unknown
// PTHREAD: remark: wtc: a queue escapes this translation unit (it is handed, with the address of push_elsewhere_c, whose body is not here, to pthread_create)
// PTHREAD: remark: wtc: queue @_ZL1q: unknown
// FNPTR: remark: wtc: a queue escapes this translation unit (it is handed to a call through a pointer, which may reach hand_off(wtc::queue<int>*), whose body is not here)
// FNPTR: remark: wtc: queue @_ZL1q: unknown
// EXTERN: remark: wtc: a queue escapes this translation unit (it is reachable from the global q, and it is defined in another translation unit)
// EXTERN: remark: wtc: queue @q: unknown
// KEPT-NOT: escapes this translation unit
// KEPT: remark: wtc: queue @_ZL1q: spsc (producers: 1, consumers: 1)

#include "queue.hpp"
#include "thread.hpp"

#include <cstdint>
#include <pthread.h>
#include <thread>

#if defined(EXTERN)
extern wtc::queue<int> q;   // defined in another file
#else
static wtc::queue<int> q;
#endif

#if defined(POINTER) || defined(FNPTR)
void hand_off(wtc::queue<int>* q);   // defined in another file
#elif defined(REFERENCE)
void hand_off_ref(wtc::queue<int>& q);
#elif defined(STRUCT)
struct params {
    int n;
    wtc::queue<int>* q;
};
void start_elsewhere(params* p);
#elif defined(VOID)
extern "C" void register_handle(void* h);
#elif defined(RELAY)
extern "C" void register_handle(void* h);
static void relay(void* h) { register_handle(h); }
#elif defined(HEAPBOX)
extern "C" void register_handle(void* h);
struct box {
    int n;
    void* p;
};
#elif defined(PARAMSTORE)
extern "C" void register_handle(void* h);
static void stash(void** slot, void* v) { *slot = v; }
#elif defined(CHAIN)
extern "C" void register_handle(void* h);
static void *g1, *g2, *g3, *g4, *g5, *g6;
#elif defined(BYVALUE)
struct params {
    int n;
    wtc::queue<int>* q;
};
void start_by_value(params p);
#elif defined(INTEGER)
extern "C" void register_word(std::uintptr_t w);
#elif defined(FIELD)
struct context {
    int n;
    void* user;
};
void start_context(context* c);
#elif defined(SPAWN)
void push_elsewhere(void* q);
#elif defined(STDTHREAD)
void push_elsewhere_typed(wtc::queue<int>* q);
#elif defined(PTHREAD)
extern "C" void* push_elsewhere_c(void* q);
#elif defined(EXTERN)
#else
static int kept;
void keep(wtc::queue<int>*) { ++kept; }
#endif

void producer() { for (int i = 0; i < 100; ++i) q.push(i); }

int main() {
#if defined(FNPTR)
    void (*volatile pick)(wtc::queue<int>*) = hand_off;
    pick(&q);
#elif defined(POINTER)
    hand_off(&q);
#elif defined(REFERENCE)
    hand_off_ref(q);
#elif defined(STRUCT)
    params p{100, &q};
    start_elsewhere(&p);
#elif defined(VOID)
    register_handle(&q);
#elif defined(RELAY)
    relay(&q);
#elif defined(HEAPBOX)
    box* b = new box;
    b->p = &q;
    register_handle(b);
#elif defined(PARAMSTORE)
    void* s = nullptr;
    stash(&s, &q);
    register_handle(s);
#elif defined(CHAIN)
    // Each copy reads what the next line of the previous iteration wrote:
    // one step of the search per link.
    for (int i = 0; i < 6; ++i) {
        g6 = g5; g5 = g4; g4 = g3; g3 = g2; g2 = g1;
        g1 = &q;
    }
    register_handle(g6);
#elif defined(BYVALUE)
    start_by_value(params{100, &q});
#elif defined(INTEGER)
    register_word(reinterpret_cast<std::uintptr_t>(&q));
#elif defined(FIELD)
    context c{100, &q};
    start_context(&c);
#elif defined(SPAWN)
    auto u = wtc::spawn(push_elsewhere, &q);
    u.join();
#elif defined(STDTHREAD)
    std::thread u(push_elsewhere_typed, &q);
    u.join();
#elif defined(PTHREAD)
    pthread_t u;
    pthread_create(&u, nullptr, push_elsewhere_c, &q);
    pthread_join(u, nullptr);
#elif defined(EXTERN)
#else
    keep(&q);
#endif
    auto t = wtc::spawn(producer);
    int got = 0;
    while (got < 100)
        if (q.try_pop()) ++got;
    t.join();
}
