/* For declared_types.c: the definitions. */
int table[4] = {1, 2, 3, 4};
const char *names[] = {"a", "b"};
int sum(void) { int s = 0; for (int i = 0; i < 4; ++i) s += table[i]; return s; }
