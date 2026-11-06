/* For declared_types.c: a common symbol larger than the other file's. */
int buf[16];
void fill(void) { for (int i = 0; i < 16; ++i) buf[i] = i + 1; }
