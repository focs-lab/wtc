// --unity in C++: an earlier file's overload changes which function a
// later file calls. Compiled apart, main's show(2.5) converts to int and
// calls its own show(int); in one unit, the earlier file's show(double)
// is a better match. The IR of main differs, and the build stops.
//
// RUN: %clangxx -O1 -w %S/Inputs/unity_overload_a.cpp %s -o %t.exe
// RUN: %t.exe | FileCheck %s --check-prefix=OUT
// RUN: not %wtc_cc --lift --unity -O1 -w %S/Inputs/unity_overload_a.cpp %s -o %t.u.exe 2>&1 | FileCheck %s
//
// OUT: double 1.5
// OUT: int 2
// CHECK: wtc-cc: --unity: a file means something else in the unity than compiled apart; compile it apart
// CHECK: wtc-cc: --unity: {{.*}}unity_overload.cpp: @main reads differently in the unity:

#include <cstdio>
void a_run();
static void show(int x) { std::printf("int %d\n", x); }
int main() {
    a_run();
    show(2.5);
    return 0;
}
