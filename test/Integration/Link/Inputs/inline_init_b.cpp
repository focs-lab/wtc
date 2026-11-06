#include "inline_init.h"
int bump_b() { (void)Holder<int>::n.v; return ++counter(); }
