/* Several C files, linked as CIR modules (wtc-link): a global one file
   declares with another type than the one kept, an array of no size for
   the definition's four elements, or a common symbol of two sizes
   (-fcommon), of which the link keeps the largest, as the linker does.
   The file reads it through its own type; before, the module did not
   verify.

   RUN: %clang -O1 %s %S/Inputs/declared_types_b.c -o %t.ref.exe
   RUN: %t.ref.exe | FileCheck %s --check-prefix=ARRAY
   RUN: %wtc_cc -O1 %s %S/Inputs/declared_types_b.c -o %t.exe
   RUN: %t.exe | FileCheck %s --check-prefix=ARRAY
   RUN: %clang -O1 -fcommon -DCOMMON %s %S/Inputs/declared_types_common.c -o %t.cref.exe
   RUN: %t.cref.exe | FileCheck %s --check-prefix=COMMON
   RUN: %wtc_cc -O1 -fcommon -DCOMMON %s %S/Inputs/declared_types_common.c -o %t.c.exe
   RUN: %t.c.exe | FileCheck %s --check-prefix=COMMON

   ARRAY: 3 10 b
   COMMON: 1 2
*/
#include <stdio.h>
#ifdef COMMON
int buf[2];
void fill(void);
int main(void) { fill(); printf("%d %d\n", buf[0], buf[1]); return 0; }
#else
extern int table[];
extern const char *names[];
int sum(void);
int main(void) { printf("%d %d %s\n", table[2], sum(), names[1]); return 0; }
#endif
