/* Several C files, linked as CIR modules (wtc-link): the second file's
   api is an alias of its own static impl, which the front end does not
   emit. The name left unresolved in its file is external, never another
   file's static: the first file's impl is renamed out of its way, and the
   link stops with a reason, where it bound api to the first file's impl
   (the program printed 1 1 for 1 2).

   RUN: %clang -O1 %s %S/Inputs/dangling_alias_b.c -o %t.ref.exe
   RUN: %t.ref.exe | FileCheck %s --check-prefix=OUT
   RUN: rm -f %t.exe
   RUN: not %wtc_cc -O1 %s %S/Inputs/dangling_alias_b.c -o %t.exe 2>&1 | FileCheck %s
   RUN: test ! -e %t.exe

   OUT: 1 2
   CHECK: wtc-link: api is an alias of impl, which its file does not define
*/
#include <stdio.h>
static int impl(void) { return 1; }
int api(void);
int main(void) { printf("%d %d\n", impl(), api()); return 0; }
