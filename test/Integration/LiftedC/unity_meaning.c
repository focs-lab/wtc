/* --unity holds a file to what it means compiled alone, not only to what
   it reads: its IR, alone and in the unit, must define the same functions
   and globals identically, and none of its names may bind to another
   file's static. A file before this one that leaves #pragma pack open
   (PACK) changes the size this file computes of its own structure; one
   that has a static of the name this file declares extern (STATIC) gives
   it another variable. Compiled as one, each printed something else than
   the files built apart; the build now stops, naming the file and what
   differs. Files that mean the same (unity.c, unity_macros.c) build.

   RUN: %clang -O1 -DPACK %S/Inputs/unity_pack.c %s -o %t.pack.exe
   RUN: %t.pack.exe | FileCheck %s --check-prefix=PACK-OUT
   RUN: not %wtc_cc --lift --unity -O1 -DPACK %S/Inputs/unity_pack.c %s -o %t.u1.exe 2>&1 | FileCheck %s --check-prefix=PACK
   RUN: %clang -O1 -DSTATIC %S/Inputs/unity_static.c %s %S/Inputs/unity_limit.c -o %t.static.exe
   RUN: %t.static.exe | FileCheck %s --check-prefix=STATIC-OUT
   RUN: %clang -O1 -c %S/Inputs/unity_limit.c -o %t.limit.o
   RUN: not %wtc_cc --lift --unity -O1 -DSTATIC %S/Inputs/unity_static.c %s %t.limit.o -o %t.u2.exe 2>&1 | FileCheck %s --check-prefix=STATIC

   PACK-OUT: wire 5 rec 8
   PACK: wtc-cc: --unity: a file means something else in the unity than compiled apart; compile it apart
   PACK: wtc-cc: --unity: {{.*}}unity_meaning.c: @main reads differently in the unity:
   STATIC-OUT: own 3 limit 7
   STATIC: wtc-cc: --unity: {{.*}}unity_meaning.c: @limit, which it only declares, is a static of another file in the unity
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
