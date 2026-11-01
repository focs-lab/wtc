/* Known open finding (docs/lifting.md, "Known open findings"): a header
   with an include guard and a static of its own. Each file that includes
   it, compiled apart, has its own next_id and its own counter; in the
   unity the second include is skipped, and both files share them. The
   check that each file means in the unity what it means alone does not
   catch it. The test passes once wtc-cc --unity refuses the files, or the
   unity keeps what the files print compiled apart.

   XFAIL: *
   RUN: rm -f %t.exe
   RUN: %wtc_cc --unity -O1 %S/Inputs/open_unity_header_static_a.c %s -o %t.exe > %t.log 2>&1 || grep -q "wtc-cc: --unity:" %t.log
   RUN: test ! -e %t.exe || %t.exe | FileCheck %s --match-full-lines

   CHECK: a 1 2 b 1
*/
#include <stdio.h>
#include "Inputs/open_unity_header_static.h"

int a_id(void); /* Inputs/open_unity_header_static_a.c */

int main(void) {
  int a1 = a_id(), a2 = a_id();
  printf("a %d %d b %d\n", a1, a2, next_id());
  return 0;
}
