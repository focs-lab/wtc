// The other file of escape_ctor_register.cpp, built as written at -O2.
#include <cstdio>
#include <queue>
static std::queue<int> *watched;
extern "C" void watch(void *p) { watched = static_cast<std::queue<int> *>(p); }
extern "C" void report() { std::printf("size %zu\n", watched->size()); }
