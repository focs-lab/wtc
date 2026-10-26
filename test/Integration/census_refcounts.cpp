// wtc-cc --census end to end: a program that defines wtc_rc_note itself
// gets its own definition called at every reference-count operation, with
// the operation's number, which is how the census library counts them.
// The control block's making, ten copies and their releases, a weak_ptr
// made, locked and dropped, and the last release, which destroys the block.
//
// RUN: %wtc_cc --census %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=PASS
// RUN: %t.exe | FileCheck %s
// RUN: %wtc_cc --census -O2 %s -o %t.o2.exe
// RUN: %t.o2.exe | FileCheck %s
//
// PASS: wtc-census: reference-count functions instrumented
// CHECK: copy 10 release 12 lock 1 weak_copy 1 weak_release 1 destroy 1 make 1

#include <atomic>
#include <cstdio>
#include <memory>

static std::atomic<int> counts[7];

extern "C" void wtc_rc_note(void*, int op) {
    if (op >= 0 && op < 7) counts[op].fetch_add(1, std::memory_order_relaxed);
}

int main() {
    auto p = std::make_shared<int>(1);
    for (int i = 0; i < 10; ++i) {
        auto q = p;
        (void)q;
    }
    std::weak_ptr<int> w = p;
    auto l = w.lock();
    l.reset();
    w.reset();
    p.reset();
    std::printf("copy %d release %d lock %d weak_copy %d weak_release %d destroy %d make %d\n",
                counts[0].load(), counts[1].load(), counts[2].load(), counts[3].load(),
                counts[4].load(), counts[5].load(), counts[6].load());
    return 0;
}
