/* A file that ends inside #pragma pack(push, 1): compiled apart, the pragma
   ends with it; in a unity, it packs the structures of every later file. */
#include <stdio.h>
#pragma pack(push, 1)
struct wire {
  char tag;
  int len;
};
int wire_size(void) { return (int)sizeof(struct wire); }
