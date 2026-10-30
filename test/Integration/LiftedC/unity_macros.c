/* --unity compiles several files as one, and each must mean what it means
   compiled apart. A macro a file defines in its own text is undefined
   after it, so that it does not reach the files after it: here LIMIT,
   which would replace the second file's default (5 apart, 9 in the
   unity). One a header of it defines cannot be, and when it changes a
   later file's text the build stops. A feature-test macro configures the
   system's headers once for every later file: refused.

   RUN: %wtc_cc --unity -O1 %s %S/Inputs/unity_macro_user.c -o %t.exe
   RUN: %t.exe | FileCheck %s
   RUN: not %wtc_cc --unity -O1 -DIN_HEADER -I%S/Inputs %s %S/Inputs/unity_macro_user.c -o %t.h.exe 2>&1 | FileCheck %s --check-prefix=HEADER
   RUN: not %wtc_cc --unity -O1 -DFEATURE %s %S/Inputs/unity_macro_user.c -o %t.f.exe 2>&1 | FileCheck %s --check-prefix=FEATURE

   CHECK: limit 5
   HEADER: wtc-cc: --unity: {{.*}}unity_macro_user.c reads differently after the files before it (a macro of theirs changes it); compile it apart
   FEATURE: wtc-cc: --unity: {{.*}}unity_macros.c defines _GNU_SOURCE, which configures the headers of every file after it; pass it with -D instead
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
