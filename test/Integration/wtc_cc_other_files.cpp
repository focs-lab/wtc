// wtc-cc tells the analysis what the program's other files may name. Linked
// with an object file that names this file's queue (PUSHES), the queue
// escapes: that file may push to it on threads the analysis never sees,
// and here one does, so the single-producer ring would race. It keeps the
// general-purpose implementation, and the program runs correctly, also
// under ThreadSanitizer. Linked with a file that names nothing of this
// one (QUIET), the queue is specialised as before. Compiled with -c, any
// file may be linked with it later, and the queue escapes (OBJECT).
//
// RUN: %clangxx -std=c++17 -I%shim -O1 -c %S/Inputs/other_file_pushes.cpp -o %t.pushes.o
// RUN: %clangxx -std=c++17 -O1 -c %S/Inputs/other_file_quiet.cpp -o %t.quiet.o
// RUN: %wtc_cc -DWTC_CHECK %s %t.pushes.o -o %t.pushes.exe 2>&1 | FileCheck %s --check-prefix=PUSHES
// RUN: %t.pushes.exe
// RUN: %wtc_cc -DQUIET %s %t.quiet.o -o %t.quiet.exe 2>&1 | FileCheck %s --check-prefix=QUIET
// RUN: %t.quiet.exe
// RUN: %wtc_cc -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=OBJECT
// RUN: %if tsan %{ %clangxx -std=c++17 -I%shim -O1 -g -fsanitize=thread -c %S/Inputs/other_file_pushes.cpp -o %t.pushes.tsan.o %}
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s %t.pushes.tsan.o -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefix=PUSHES %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --allow-empty --check-prefix=TSAN %}
//
// PUSHES: remark: wtc: a queue escapes this translation unit (it is reachable from the global q, and another file of the program names it)
// PUSHES: remark: wtc: queue @q: unknown
// PUSHES-NOT: rewritten
// QUIET-NOT: escapes this translation unit
// QUIET: remark: wtc: queue @q: spsc (producers: 1, consumers: 1)
// QUIET: rewritten to spsc
// OBJECT: remark: wtc: a queue escapes this translation unit (it is reachable from the global q, and the unit is built to be linked with other files of the program)
// OBJECT-NOT: rewritten
// TSAN-NOT: ThreadSanitizer

#include "queue.hpp"
#include "thread.hpp"

wtc::queue<int> q;

#ifndef QUIET
void push_from_elsewhere(int n);   // in the other file
static void from_elsewhere() { push_from_elsewhere(1000); }
#endif

static void producer() { for (int i = 0; i < 1000; ++i) q.push(i); }

int main() {
    auto t = wtc::spawn(producer);
#ifndef QUIET
    auto u = wtc::spawn(from_elsewhere);
    const int expected = 2000;
#else
    const int expected = 1000;
#endif
    int got = 0;
    while (got < expected)
        if (q.try_pop()) ++got;
    t.join();
#ifndef QUIET
    u.join();
#endif
    return 0;
}
