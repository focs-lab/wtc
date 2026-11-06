/* Several C files, linked as CIR modules (wtc-link): a call through a
   declaration with no prototype, the definition in the other file (C17
   6.5.2.2). The first file's view of the function's type is cast to the
   one kept; before, the module did not verify.

   RUN: %clang -O1 -w %s %S/Inputs/kr_call_b.c -o %t.ref.exe
   RUN: %t.ref.exe | FileCheck %s
   RUN: %wtc_cc -O1 -w %s %S/Inputs/kr_call_b.c -o %t.exe
   RUN: %t.exe | FileCheck %s

   CHECK: 42
*/
#include <stdio.h>
int scale();
int main(void) { printf("%d\n", scale(21)); return 0; }
