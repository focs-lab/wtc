/* For unity_macros.c: a file with a default of its own, which a macro of
   a file compiled before it in a unity build would replace. */
#ifndef LIMIT
#define LIMIT 5
#endif
int limit_of(void) { return LIMIT; }
