/* The other file of link_struct_tags.c: a struct node of its own, and an
   anonymous struct before the system's headers, which shifts the numbers
   the front end gives theirs (anon.N). */
static struct {
  char c;
} early = {1};
#include <pthread.h>
struct node {
  int key;
  int pad;
};
struct list {
  struct node first;
};
static struct list items = {{3, 0}};
static int seen(void) { return items.first.key; }

static pthread_cond_t cv;
int a_report(int *size) {
  pthread_cond_init(&cv, 0);
  pthread_cond_destroy(&cv);
  *size = (int)sizeof(struct list);
  return seen() + early.c - 1;
}
