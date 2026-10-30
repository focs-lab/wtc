// A file with a file-local overload of a name the later file also
// declares file-local: in a unity both are seen, and overload resolution
// in the later file picks this one.
#include <cstdio>
static void show(double x) { std::printf("double %.1f\n", x); }
void a_run() { show(1.5); }
