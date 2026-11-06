// The other file of escape_section.cpp, built as written at -O2.
#include "other_unit.h"
extern "C" void *__start_wtcreg[], *__stop_wtcreg[];
extern "C" std::size_t their_pending_registered() {
    std::size_t n = 0;
    for (void **p = __start_wtcreg; p < __stop_wtcreg; ++p) n += static_cast<LQ<int> *>(*p)->pending();
    return n;
}
