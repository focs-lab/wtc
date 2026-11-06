// The other file of escape_link_inputs.cpp, built as written at -O2, as an
// object file and as a shared object.
#include "other_unit.h"
extern "C" std::size_t their_pending(void *q) { return static_cast<LQ<int> *>(q)->pending(); }
