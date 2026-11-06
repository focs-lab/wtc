/* Several files, linked as CIR modules (wtc-cc, wtc-link), each compiled
   apart: a macro a file defines, in its own text or in a header of its
   own, does not reach the files after it (LIMIT would replace the second
   file's default, 5 apart, 9 together), and a feature-test macro
   configures the system's headers for its own file only. Concatenated
   into one unit (--unity until 6 Nov 2026), the first was undefined after
   the file and the others stopped the build; linked, all three build and
   print what they print built apart.

   RUN: %wtc_cc --lift --unity -O1 %s %S/Inputs/unity_macro_user.c -o %t.exe
   RUN: %t.exe | FileCheck %s
   RUN: %wtc_cc --lift -O1 -DIN_HEADER -I%S/Inputs %s %S/Inputs/unity_macro_user.c -o %t.h.exe
   RUN: %t.h.exe | FileCheck %s
   RUN: %wtc_cc --lift -O1 -DFEATURE %s %S/Inputs/unity_macro_user.c -o %t.f.exe
   RUN: %t.f.exe | FileCheck %s

   CHECK: limit 5
*/
#if defined(FEATURE)
#define _GNU_SOURCE
#endif
#include <stdio.h>
#if defined(IN_HEADER)
#include "unity_macro.h"
#elif !defined(FEATURE)
#define LIMIT 9
#endif

int limit_of(void);

int main(void) {
  printf("limit %d\n", limit_of());
  return 0;
}
