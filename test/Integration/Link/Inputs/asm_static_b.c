/* For asm_static.c: a static its inline assembly names. */
static int counter __attribute__((used)) = 2;
int fb(void) {
  int r;
  __asm__("movl counter(%%rip), %0" : "=r"(r));
  return r;
}
