// Several C++ files, linked as CIR modules (wtc-cc, wtc-link): each file's
// statics, its anonymous namespace and its dynamic initialisation stay its
// own, and an inline function or a template instantiated in both is one
// function for the program, as the linker keeps one copy.
//
// RUN: %clangxx -std=c++17 -O1 %S/Inputs/link_globals_a.cpp %s -o %t.ref.exe
// RUN: %t.ref.exe | FileCheck %s
// RUN: %wtc_cc -O1 %S/Inputs/link_globals_a.cpp %s -o %t.exe
// RUN: %t.exe | FileCheck %s
//
// CHECK: a 11 b 21 2 init b

#include <cstdio>
#include <string>

static int hits;
namespace {
struct K {
    long w, z;
};
K k{2, 3};
}  // namespace
std::string init_b = "init b";
inline int shared() { return 7; }
template <class T> T twice(T x) { return x + x; }
int a_hits();

int main() {
    hits = 10;
    std::printf("a %d b %d %ld %s\n", a_hits(), hits + twice(2) + shared(), k.w, init_b.c_str());
}
