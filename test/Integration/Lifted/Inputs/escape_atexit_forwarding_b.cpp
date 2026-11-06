// The other file of escape_atexit_forwarding.cpp, built as written at -O2.
#include "other_unit.h"
#include <cstdio>

extern "C" void report_left(void* p) { std::printf("left %zu\n", static_cast<LQ<int>*>(p)->pending()); }
