// The other file of other_unit_inlined.cpp, built as written at -O2: its
// copy of LQ<int>::pending() is inlined into report(), and nm lists no
// symbol that names LQ.
#include "other_unit.h"

#if defined(GLOBAL)
extern LQ<int> gq;
std::size_t report() { return gq.pending(); }
#else
extern "C" std::size_t report(void* q) { return static_cast<LQ<int>*>(q)->pending(); }
#endif
