/* Several C files, linked as CIR modules (wtc-cc, wtc-link): a struct tag
   is its file's own. Both files define `struct node`, each its own way,
   and a static of the same name; linked, the second file's struct and the
   records that hold it are renamed for it, as are its statics, and each
   file reads its own. The system's anonymous structs, numbered otherwise
   in each file, are one type (pthread_cond_t, used by both).

   RUN: %clang -O1 %S/Inputs/link_struct_tags_a.c %s -o %t.ref.exe
   RUN: %t.ref.exe | FileCheck %s
   RUN: %wtc_cc -O1 %S/Inputs/link_struct_tags_a.c %s -o %t.exe
   RUN: %t.exe | FileCheck %s

   CHECK: a 3 8 b 7 16 24
*/
#include <pthread.h>
#include <stdio.h>

struct node {
  long key;
  struct node *next;
};
struct list {
  struct node first;
  long n;
};
static struct list items = {{7, 0}, 1};
static int seen(void) { return (int)items.first.key; }

int a_report(int *size);

int main(void) {
  pthread_cond_t cv;
  pthread_cond_init(&cv, 0);
  pthread_cond_destroy(&cv);
  int asize;
  int a = a_report(&asize);
  printf("a %d %d b %d %zu %zu\n", a, asize, seen(), sizeof(struct node), sizeof(struct list));
  return 0;
}
