/* Known open finding (docs/lifting.md, "Known open findings"): each file
   keeps a counter of its own, `static int hits;`. In the unity the two
   are tentative definitions of one internal object (C11 6.9.2), so the
   files share it; compiled apart, they do not. The check that each file
   means in the unity what it means alone does not catch it. The test passes
   once wtc-cc --unity refuses the files, or the unity keeps what the files
   print compiled apart.

   XFAIL: *
   RUN: rm -f %t.exe
   RUN: %wtc_cc --unity -O1 %S/Inputs/open_unity_tentative_a.c %s -o %t.exe > %t.log 2>&1 || grep -q "wtc-cc: --unity:" %t.log
   RUN: test ! -e %t.exe || %t.exe | FileCheck %s --match-full-lines

   CHECK: a 2 b 10
*/
#include <stdio.h>

static int hits;
void a_hit(void);  /* Inputs/open_unity_tentative_a.c */
int a_hits(void);

int main(void) {
  a_hit();
  a_hit();
  hits = 10;
  printf("a %d b %d\n", a_hits(), hits);
  return 0;
}
