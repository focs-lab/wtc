/* Several files, linked as CIR modules (wtc-cc, wtc-link), mean what they
   mean compiled apart. A file before this one that leaves #pragma pack
   open (PACK) does not change the size this file computes of its own
   structure, and one with a static of the name this file declares extern
   (STATIC) does not give it another variable. Concatenated into one unit
   (--unity until 6 Nov 2026), each printed something else than the files
   built apart, and the build had to stop; linked, each prints what the
   files print built apart.

   RUN: %clang -O1 -DPACK %S/Inputs/unity_pack.c %s -o %t.pack.exe
   RUN: %t.pack.exe | FileCheck %s --check-prefix=PACK-OUT
   RUN: %wtc_cc --lift --unity -O1 -DPACK %S/Inputs/unity_pack.c %s -o %t.u1.exe
   RUN: %t.u1.exe | FileCheck %s --check-prefix=PACK-OUT
   RUN: %clang -O1 -DSTATIC %S/Inputs/unity_static.c %s %S/Inputs/unity_limit.c -o %t.static.exe
   RUN: %t.static.exe | FileCheck %s --check-prefix=STATIC-OUT
   RUN: %clang -O1 -c %S/Inputs/unity_limit.c -o %t.limit.o
   RUN: %wtc_cc --lift -O1 -DSTATIC %S/Inputs/unity_static.c %s %t.limit.o -o %t.u2.exe
   RUN: %t.u2.exe | FileCheck %s --check-prefix=STATIC-OUT

   PACK-OUT: wire 5 rec 8
   STATIC-OUT: own 3 limit 7
*/
#include <stdio.h>

#if defined(PACK)
int wire_size(void);
struct rec {
  char c;
  int i;
};
int main(void) {
  printf("wire %d rec %zu\n", wire_size(), sizeof(struct rec));
  return 0;
}
#else
int own_limit(void);
extern int limit;
int main(void) {
  printf("own %d limit %d\n", own_limit(), limit);
  return 0;
}
#endif
