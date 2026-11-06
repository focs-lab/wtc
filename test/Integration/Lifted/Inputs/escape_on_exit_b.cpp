// The other file of escape_on_exit.cpp, built as written at -O2.
#include "other_unit.h"
#include <cstdint>
extern "C" std::size_t their_pending(void *q) { return static_cast<LQ<int> *>(q)->pending(); }
