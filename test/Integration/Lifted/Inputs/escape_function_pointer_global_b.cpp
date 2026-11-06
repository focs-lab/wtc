// The other file of escape_function_pointer_global.cpp, built as written at -O2.
#include "other_unit.h"

extern LQ<int>* (*g_get)();
std::size_t their_pending() { return g_get()->pending(); }
