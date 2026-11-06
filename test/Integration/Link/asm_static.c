/* Several C files, linked as CIR modules (wtc-link): both files have a
   static `counter`, and the second's inline assembly names its own by text.
   The link renames a static whose name another file has, in every use the
   symbol table shows, which the assembly's text is not; it now stops with
   a reason instead, where the second file's assembly read the first's
   counter (the program printed 1 1 for 1 2).

   RUN: %clang -O1 %s %S/Inputs/asm_static_b.c -o %t.ref.exe
   RUN: %t.ref.exe | FileCheck %s --check-prefix=OUT
   RUN: rm -f %t.exe
   RUN: not %wtc_cc -O1 %s %S/Inputs/asm_static_b.c -o %t.exe 2>&1 | FileCheck %s
   RUN: test ! -e %t.exe

   OUT: 1 2
   CHECK: wtc-link: counter, a static of {{.*}} that its assembly names, has the name of another file's symbol
*/
#include <stdio.h>
static int counter = 1;
int fa(void) { return counter; }
int fb(void);
int main(void) { printf("%d %d\n", fa(), fb()); return 0; }
