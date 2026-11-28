// A virtual call in a section is resolved by the module's vtables only
// when the module is the whole program: compiled to be linked later
// (open-world, what wtc-cc -c passes) or linked with files that define or
// name symbols (named-elsewhere, what wtc-cc lists from them), another
// file may define a class derived from the call's, whose overrider the
// pass does not see.
//
// RUN: %clangxx -std=c++17 -O1 -fno-exceptions -fclangir -emit-cir %s -o %t.cir
// RUN: wtc-opt %t.cir --wtc-delegate-sections=mutex=T::mu_ -o /dev/null 2>&1 | FileCheck %s --check-prefix=WHOLE
// RUN: wtc-opt %t.cir --wtc-delegate-sections="mutex=T::mu_ open-world=true" -o /dev/null 2>&1 | FileCheck %s
// RUN: echo _Z5otherv > %t.named
// RUN: wtc-opt %t.cir --wtc-delegate-sections="mutex=T::mu_ named-elsewhere=%t.named" -o /dev/null 2>&1 | FileCheck %s
//
// WHOLE: remark: wtc: delegated section T::run #1 on T::mu_ delegated
// CHECK: remark: wtc: delegated section T::run #1 on T::mu_ not delegated: (e) makes a virtual call on Base the pass cannot resolve

#include <mutex>

struct Base {
    virtual long get() noexcept = 0;
    virtual ~Base() = default;
};
struct One : Base {
    long get() noexcept override { return 1; }
};

struct T {
    std::mutex mu_;
    long n = 0;
    Base *b = nullptr;
    void run() noexcept {
        std::lock_guard<std::mutex> g(mu_);
        n += b->get();
    }
};

int main() {
    T t;
    One one;
    t.b = &one;
    t.run();
    return 0;
}
