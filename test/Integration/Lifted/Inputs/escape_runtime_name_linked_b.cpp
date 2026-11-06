// The other file of escape_runtime_name_linked.cpp, built as written at -O2.
#include <cstdio>
#include <queue>
static std::queue<int> *view;
extern "C" void wtc_note(void *p) { view = static_cast<std::queue<int> *>(p); }
extern "C" void wtc_report() { std::printf("size %zu\n", view->size()); }
