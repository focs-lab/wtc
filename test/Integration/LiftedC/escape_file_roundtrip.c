/* A finding of the review of 23 Nov 2026: C: a helper that writes a
   pointer to a struct holding the queue to a file and reads it back with
   pread. Lifted, the program printed otherwise; now the struct is refused,
   and the program prints what it prints as written.

   RUN: %wtc_cc --lift -O1 -DWTC_CHECK -I%S/../../Transforms/LiftCQueues/Inputs %s -o %t.exe 2>&1 | FileCheck %s --check-prefix=WHY
   RUN: %t.exe | FileCheck %s --match-full-lines

   CHECK: put 3
   CHECK: sum 6
   WHY: C queue struct chan not lifted:
*/
#include "chan.h"
#include <stdarg.h>
#include <unistd.h>

struct app { struct chan q; int id; };
static struct app A;

static int peek(int fd, ...) {
  va_list ap;
  va_start(ap, fd);
  void *p = va_arg(ap, void *);
  va_end(ap);
  if (write(fd, &p, sizeof p) != sizeof p) return -1;
  int *back = NULL;
  if (pread(fd, &back, sizeof back, 0) != sizeof back) return -1;
  return back[0]; /* struct chan's put index, as written */
}

int main(void) {
  chan_open(&A.q, 8, 1);
  for (long i = 1; i <= 3; ++i) chan_send(&A.q, (void *)i);
  FILE *f = tmpfile();
  /* not printf of the value read, which would join the files to the outside */
  puts(peek(fileno(f), &A) == 3 ? "put 3" : "put not 3");
  void *v;
  long s = 0;
  for (int i = 0; i < 3; ++i) { chan_recv(&A.q, &v); s += (long)v; }
  printf("sum %ld\n", s);
  chan_writer_done(&A.q);
  chan_close(&A.q);
  return 0;
}
