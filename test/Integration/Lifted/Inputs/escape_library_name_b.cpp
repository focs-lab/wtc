// The other file of escape_library_name.cpp, built as written at -O2.
#include "other_unit.h"
extern "C" std::size_t sem_pending(void *q) { return static_cast<LQ<int> *>(q)->pending(); }
