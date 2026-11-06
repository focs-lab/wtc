/* Several files, linked as CIR modules (wtc-cc, wtc-link): a header with
   an include guard and a static of its own. Each file that includes it,
   compiled apart, has its own next_id and its own counter. Concatenated
   into one unit (--unity until 6 Nov 2026), the second include was
   skipped and both files shared them, which no check of the unit caught
   (a finding of 1 Nov 2026). Linked, each file keeps its own: the second
   file's statics are renamed, and the program prints what the files
   print built apart.

   RUN: %wtc_cc -O1 %S/Inputs/unity_header_static_a.c %s -o %t.exe
   RUN: %t.exe | FileCheck %s --match-full-lines
   RUN: %wtc_cc --unity -O1 %S/Inputs/unity_header_static_a.c %s -o %t.u.exe
   RUN: %t.u.exe | FileCheck %s --match-full-lines

   CHECK: a 1 2 b 1
*/
#include <stdio.h>
#include "Inputs/unity_header_static.h"

int a_id(void); /* Inputs/unity_header_static_a.c */

int main(void) {
  int a1 = a_id(), a2 = a_id();
  printf("a %d %d b %d\n", a1, a2, next_id());
  return 0;
}
