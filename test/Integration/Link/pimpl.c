/* Several C files, linked as CIR modules (wtc-link): the pimpl idiom,
   struct impl complete only in the second file. A record is compared with
   another file's by the names of the records its pointers point to, not by
   whether the file completes them; before, the struct holding the pointer
   was taken for another file's and the module did not verify.

   RUN: %clang -O1 -I%S/Inputs %s %S/Inputs/pimpl_b.c -o %t.ref.exe
   RUN: %t.ref.exe | FileCheck %s
   RUN: %wtc_cc -O1 -I%S/Inputs %s %S/Inputs/pimpl_b.c -o %t.exe
   RUN: %t.exe | FileCheck %s

   CHECK: 42
*/
#include <stdio.h>
#include "pimpl.h"
int main(void) {
  struct widget w;
  widget_init(&w);
  printf("%d\n", widget_value(&w));
  return 0;
}
