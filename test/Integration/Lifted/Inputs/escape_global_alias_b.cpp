// The other file of escape_global_alias.cpp, built as written at -O2.
#include "other_unit.h"
extern "C" LQ<int> gq_alias;
extern "C" std::size_t their_pending_alias() { return gq_alias.pending(); }
