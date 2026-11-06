/* Several files, linked as CIR modules (wtc-cc, wtc-link): each file
   keeps a counter of its own, `static int hits;`. Concatenated into one
   unit (--unity until 6 Nov 2026), the two were tentative definitions of
   one internal object (C11 6.9.2), and the files shared it (a finding of
   1 Nov 2026). Linked, each file keeps its own, as compiled apart.

   RUN: %wtc_cc -O1 %S/Inputs/unity_tentative_a.c %s -o %t.exe
   RUN: %t.exe | FileCheck %s --match-full-lines

   CHECK: a 2 b 10
*/
#include <stdio.h>

static int hits;
void a_hit(void);  /* Inputs/unity_tentative_a.c */
int a_hits(void);

int main(void) {
  a_hit();
  a_hit();
  hits = 10;
  printf("a %d b %d\n", a_hits(), hits);
  return 0;
}
