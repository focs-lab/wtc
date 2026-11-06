// The other file of escape_foreign_storage.cpp, built as written at -O2.
#include "other_unit.h"

alignas(LQ<int>) static unsigned char buf[sizeof(LQ<int>)];
extern "C" void* their_storage() { return buf; }
extern "C" std::size_t their_pending() { return reinterpret_cast<LQ<int>*>(buf)->pending(); }
