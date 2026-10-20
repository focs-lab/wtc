// A consumer that only borrows, of a queue whose element type cannot be
// held inline: destroying a std::string runs code, and held inline it
// would run at the consumer's next pop instead of when the element is
// done with, which a program can see. The shim's constructor says which
// types it can hold inline, and for this one it does not, so the queue is
// plain spsc, borrowed or not, and the program's output is unchanged.
//
// RUN: %wtc_cc %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=BUILD
// RUN: %t.exe | FileCheck %s
// RUN: %if tsan %{ %wtc_cc --tsan -O1 -g %s -o %t.tsan.exe 2>&1 | FileCheck %s --check-prefix=BUILD %}
// RUN: %if tsan %{ env TSAN_OPTIONS=halt_on_error=1:symbolize=0 %t.tsan.exe 2>&1 | FileCheck %s --check-prefixes=TSAN,CHECK %}
//
// BUILD: queue @q: every pop only borrows its element
// BUILD: every pop borrows, but this element type cannot be held inline; plain spsc
// BUILD: tag rewritten to spsc{{$}}
// TSAN-NOT: ThreadSanitizer
// CHECK: tag 1 chars 3000

#include "queue.hpp"
#include "thread.hpp"

#include <cstdio>
#include <string>

struct message {
    std::string text;
};

wtc::queue<message> q;
long chars = 0;

void produce() {
    for (int i = 0; i < 1000; ++i) q.push(message{"abc"});
}

void consume() {
    for (int got = 0; got < 1000;) {
        auto p = q.try_pop();
        if (!p) continue;
        chars += static_cast<long>(p->text.size());
        ++got;
    }
}

int main() {
    std::thread a = wtc::spawn(produce);
    std::thread b = wtc::spawn(consume);
    a.join();
    b.join();
    std::printf("tag %d chars %ld\n", static_cast<int>(q.tag()), chars);
}
