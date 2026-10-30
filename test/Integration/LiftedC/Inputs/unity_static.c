/* A file with a static whose name a later file declares extern: compiled
   apart, the later file's name is another file's global; in a unity, it
   binds to this static. */
static int limit = 3;
int own_limit(void) { return limit; }
