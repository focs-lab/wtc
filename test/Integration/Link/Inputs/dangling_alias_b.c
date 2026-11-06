/* For dangling_alias.c: an alias of a static the front end does not emit. */
static int impl(void) { return 2; }
int api(void) __attribute__((alias("impl")));
