/* The other file of open_unity_tentative.c: a counter of its own. */
static int hits;
void a_hit(void) { hits++; }
int a_hits(void) { return hits; }
