// The other file of escape_function_alias.cpp, built as written at -O2.
#include "other_unit.h"
extern "C" void *get_q();
extern "C" std::size_t their_pending_of_get_q() { return static_cast<LQ<int> *>(get_q())->pending(); }
