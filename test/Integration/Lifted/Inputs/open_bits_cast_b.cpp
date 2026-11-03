// The other file of open_bits_cast.cpp, built as written at -O2.
#include "other_unit.h"
#include <cstdint>

extern "C" std::size_t their_pending(std::uintptr_t b) { return reinterpret_cast<LQ<int>*>(b)->pending(); }
